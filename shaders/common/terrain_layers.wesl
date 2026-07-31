// Shared terrain layer blending: turns 8 dense per-layer weights into ONE
// surface sampled from three texture_2d_arrays (albedo / normal / ARM).
//
// Two materials use this, differing only in where the weights come from:
//   - material/terrain_blend.wesl  -- per-VERTEX (layer, weight) pairs,
//     scattered into 8 slots by its vertex stage.
//   - material/terrain_cluster.wesl -- a biome SPLAT texture sampled by world
//     XZ, so the weights are independent of the cluster LOD cut.
// Slot index IS the texture-array layer index in both cases.
//
// The blend is HEIGHT-LERPED, not a linear cross-fade (Mishkinis, "Advanced
// Terrain Texture Splatting"): each layer's displacement biases its weight, so
// the taller material wins the contested band and the two interlock instead of
// dissolving into each other. Displacement rides in the ARM array's alpha --
// MaterialLibrary::LoadPack folds each pack's disp red channel in there, so
// height blending costs no extra array, binding, or fetch.

import package::common::frame::{frame};

// Size of one texture tile in world meters. Structural, so compile-time: it is
// a property of how the terrain materials are authored, not a runtime knob.
// Smaller = denser texels (2.5 m over a 1k pack ~= 410 px/m). Tighter tiling
// also repeats more visibly; killing that repetition is what TERRAIN_BREAK_TILING
// below is for.
const TERRAIN_MATERIAL_SCALE: f32 = 2.5;  // meters per texture tile

// Break up the visible repetition of the tiling textures (iq's "texture
// repetition" Technique 3, https://iquilezles.org/articles/texturerepetition/).
//
// Several virtual copies of the tiling are made by offsetting the lookup; a
// low-frequency index picks between them, so the plane is covered in patches
// that use different copies. The index is float, and we blend the two nearest
// copies, so the patch boundaries are smooth rather than seams.
//
// Compile-time: it doubles the texture fetches, and dead-strips to the plain
// path when off. TERRAIN_MATERIAL_SCALE = 2.5 m tiles means a LOT of repeats on
// screen, which is exactly what this hides.
const TERRAIN_BREAK_TILING: bool = true;

// Wavelength of the index pattern, in tiles: how big each same-virtual-copy
// patch is.
//
// This has to be on the order of ONE tile, which is less obvious than it looks:
// a patch shows its own copy of the tiling unbroken, so an 8-tile patch still
// reads as an 8x8 lattice -- measured, not guessed (8 and 3 both left a visible
// grid at 2.5 m tiles; 1.5 removed it). The cost of going small is that more of
// the surface is mid-crossfade between two copies, which softens detail; below
// ~1 the blend never settles anywhere.
const TILING_VARIATION_TILES: f32 = 1.5;

// Height-lerp transition width. Smaller = a harder, more interlocked seam;
// larger = closer to a plain weighted blend. Structural, so compile-time: it is
// a property of how the packs are authored, not a runtime knob.
const TERRAIN_HEIGHT_BLEND_DEPTH: f32 = 0.2;

// Layers below this contribute nothing visible. The cull is load-bearing for
// COST, not looks: bilinear filtering of a splat that stores two weights per
// texel can light up four slots at a biome triple point, and every extra
// non-zero slot is another 6 texture fetches.
const TERRAIN_MIN_LAYER_WEIGHT: f32 = 0.00392;  // 1/255

fn hash21(p: vec2<f32>) -> f32 {
    var q = fract(p * vec2<f32>(123.34, 345.45));
    q = q + vec2<f32>(dot(q, q + 34.345));
    return fract(q.x * q.y);
}

// Smooth value noise in [0,1]. iq samples a LUT texture here (cache-friendly
// and trivially filterable); this stays procedural to avoid another binding +
// upload for one scalar, and it is low-frequency enough that the cost is noise,
// not fetches.
fn variation_noise(p: vec2<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let a = hash21(i);
    let b = hash21(i + vec2<f32>(1.0, 0.0));
    let c = hash21(i + vec2<f32>(0.0, 1.0));
    let d = hash21(i + vec2<f32>(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The two virtual-copy offsets + the blend weight between them, for one uv.
// Computed ONCE per fragment and shared by all three arrays: albedo, normal and
// ARM must agree on which virtual copy they came from, or a fragment gets its
// normal from one copy and its colour from another.
struct TileVariation {
    offa: vec2<f32>,
    offb: vec2<f32>,
    blend: f32,
}

fn tile_variation(uv: vec2<f32>) -> TileVariation {
    var v: TileVariation;
    let k = variation_noise(uv / TILING_VARIATION_TILES);
    let index = k * 8.0;
    let i = floor(index);
    v.offa = sin(vec2<f32>(3.0, 7.0) * (i + 0.0));  // any hash works
    v.offb = sin(vec2<f32>(3.0, 7.0) * (i + 1.0));
    v.blend = fract(index);
    return v;
}

// One layer fetch, with or without the de-tiling. `blend` is passed in (not
// recomputed per array) to keep the three arrays coherent -- see TileVariation.
fn sample_layer(t: texture_2d_array<f32>, samp: sampler, uv: vec2<f32>,
                layer: u32, dx: vec2<f32>, dy: vec2<f32>, v: TileVariation,
                blend: f32) -> vec4<f32> {
    if (!TERRAIN_BREAK_TILING) {
        return textureSampleGrad(t, samp, uv, layer, dx, dy);
    }
    // Explicit gradients are mandatory: the offsets introduce discontinuities,
    // so implicit derivatives would pick a wildly wrong mip at every patch edge.
    let cola = textureSampleGrad(t, samp, uv + v.offa, layer, dx, dy);
    let colb = textureSampleGrad(t, samp, uv + v.offb, layer, dx, dy);
    return mix(cola, colb, blend);
}

struct BlendedSurface {
    albedo: vec3<f32>,
    tangent_normal: vec3<f32>,
    ao: f32,
    roughness: f32,
}

// Planar UV from world XZ (re-add cameraWorldPos so tiling is world-stable).
fn terrainPlanarUv(world_offset_pos: vec3<f32>) -> vec2<f32> {
    let world = world_offset_pos + frame.cameraWorldPos.xyz;
    return world.xz / TERRAIN_MATERIAL_SCALE;
}

// Tangent frame for the planar XZ projection above: dP/du = +X, dP/dv = +Z.
// B = cross(T, N) yields +Z for flat (+Y) ground -- cross(N, T) would give -Z
// and invert the normal map's green channel.
//
// Seeding from +X is degenerate when N is (anti)parallel to +X (a vertical
// face); fall back to +Z there purely to avoid a NaN. The planar projection is
// badly stretched on such faces anyway -- triplanar is the real fix (deferred).
fn terrainPlanarTbn(n: vec3<f32>) -> mat3x3<f32> {
    let N = normalize(n);
    var seed = vec3<f32>(1.0, 0.0, 0.0);
    if (abs(N.x) > 0.99) {
        seed = vec3<f32>(0.0, 0.0, 1.0);
    }
    let T = normalize(seed - N * dot(seed, N));
    let B = cross(T, N);
    return mat3x3<f32>(T, B, N);  // columns: worldN = T*tn.x + B*tn.y + N*tn.z
}

// Blend the array layers by their weights, height-lerped and normalized.
//
// Zero-weight layers are SKIPPED, which matters: at 8 slots x 3 arrays a naive
// unconditional loop costs 24 samples/fragment, while a real fragment usually
// blends <=2 layers (~6 samples). The skip is why this takes explicit gradients:
// `textureSample` inside non-uniform control flow is ILLEGAL (implicit
// derivatives need uniform control flow), so dx/dy are computed once by the
// caller (uniform) and fed to textureSampleGrad here.
//
// Each array index is clamped against THAT array's own layer count, so an
// 8-slot weight vector is safe against a smaller array (e.g. one layer per
// biome). The three arrays normally have equal layer counts (LoadTerrainArrays
// builds them from one pack list), but any slot left unbound falls back to an
// 8-layer default, so they can legitimately differ -- clamping them together
// against one array's count would let an index run past a shorter one (an
// out-of-range array_index is UNDEFINED in WGSL, not implicitly clamped).
//
// Two passes, because the height-lerped weights are not known until every
// active layer's height has been read. Albedo and ARM are CACHED across the
// passes, so the fetch count per active layer stays at 6 -- the same as the
// pre-height-lerp blend.
fn sampleTerrainLayers(albedo_array: texture_2d_array<f32>,
                       normal_array: texture_2d_array<f32>,
                       arm_array: texture_2d_array<f32>,
                       samp: sampler,
                       uv: vec2<f32>, w0: vec4<f32>, w1: vec4<f32>,
                       dx: vec2<f32>, dy: vec2<f32>) -> BlendedSurface {
    let max_albedo = textureNumLayers(albedo_array) - 1u;
    let max_normal = textureNumLayers(normal_array) - 1u;
    let max_arm = textureNumLayers(arm_array) - 1u;
    // Depends only on uv, so it is shared across every layer of this fragment.
    let var_ = tile_variation(uv);

    // Pass 1: resolve each active layer's de-tiling crossfade from ALBEDO (so
    // all three arrays stay on the same virtual copy) and read its height out
    // of ARM alpha.
    var w_in: array<f32, 8>;
    var blend_c: array<f32, 8>;
    var albedo_c: array<vec3<f32>, 8>;
    var arm_c: array<vec2<f32>, 8>;
    var h_c: array<f32, 8>;
    var ma = -1e9;
    for (var i = 0u; i < 8u; i = i + 1u) {
        var w = 0.0;
        if (i < 4u) {
            w = w0[i];
        } else {
            w = w1[i - 4u];
        }
        if (w <= TERRAIN_MIN_LAYER_WEIGHT) {
            w_in[i] = 0.0;
            continue;
        }
        w_in[i] = w;

        // iq boosts the contrast of the crossfade using the difference between
        // the two virtual copies, which narrows the visible transition band;
        // derive it from ALBEDO and reuse it for normal/ARM so all three stay
        // on the same copy.
        let li_a = min(i, max_albedo);
        var blend = 0.0;
        var col = vec3<f32>(0.0);
        if (TERRAIN_BREAK_TILING) {
            let ca = textureSampleGrad(albedo_array, samp, uv + var_.offa, li_a, dx, dy).rgb;
            let cb = textureSampleGrad(albedo_array, samp, uv + var_.offb, li_a, dx, dy).rgb;
            let d = ca - cb;
            blend = smoothstep(0.2, 0.8, var_.blend - 0.1 * (d.x + d.y + d.z));
            col = mix(ca, cb, blend);
        } else {
            col = textureSampleGrad(albedo_array, samp, uv, li_a, dx, dy).rgb;
        }
        blend_c[i] = blend;
        albedo_c[i] = col;

        let a = sample_layer(arm_array, samp, uv, min(i, max_arm), dx, dy,
                             var_, blend);
        arm_c[i] = vec2<f32>(a.r, a.g);
        h_c[i] = a.a;
        ma = max(ma, a.a + w);
    }

    // Pass 2: height-lerped weights, then the one remaining fetch (normal).
    // Only layers within TERRAIN_HEIGHT_BLEND_DEPTH of the tallest survive.
    ma = ma - TERRAIN_HEIGHT_BLEND_DEPTH;
    var albedo = vec3<f32>(0.0);
    var tn = vec3<f32>(0.0);
    var arm = vec2<f32>(0.0);
    var wsum = 0.0;
    for (var i = 0u; i < 8u; i = i + 1u) {
        if (w_in[i] <= 0.0) {
            continue;
        }
        let b = max(h_c[i] + w_in[i] - ma, 0.0);
        if (b <= 0.0) {
            continue;
        }
        albedo = albedo + b * albedo_c[i];
        arm = arm + b * arm_c[i];
        let n = sample_layer(normal_array, samp, uv, min(i, max_normal), dx, dy,
                             var_, blend_c[i]).rgb * 2.0 - 1.0;
        tn = tn + b * n;
        wsum = wsum + b;
    }

    let inv = 1.0 / max(wsum, 1e-4);
    var out: BlendedSurface;
    out.albedo = albedo * inv;
    out.ao = arm.x * inv;
    out.roughness = arm.y * inv;
    // Guard a zero-length blend (no weight anywhere) -> flat tangent normal.
    let tn_avg = tn * inv;
    if (dot(tn_avg, tn_avg) < 1e-8) {
        out.tangent_normal = vec3<f32>(0.0, 0.0, 1.0);
    } else {
        out.tangent_normal = normalize(tn_avg);
    }
    return out;
}
