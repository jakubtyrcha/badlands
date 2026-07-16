// Terrain-blend material (deferred): each vertex carries up to 4 compact
// (layer, weight) pairs (Uint8x4 indices + Unorm8x4 weights). The VERTEX stage
// scatters them into an 8-slot per-layer weight vector output as two vec4
// varyings, which interpolate as floats — so the mesh is INDEXABLE (shared
// vertices, no flat integer varyings). The FRAGMENT stage blends the layers of
// three texture_2d_arrays (albedo / normal / ARM) by those interpolated weights
// into ONE G-buffer sample; the single BRDF is done later by
// shaders/passes/deferred_lighting.wesl.
//
// A forward variant (blend + one inline BRDF, e.g. a future water material) can
// be added as a second fragment entry when a forward-opaque pass exists.

import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};

struct TerrainUniforms {
    modelMatrix: mat4x4<f32>,
}

@group(0) @binding(1) var albedo_array: texture_2d_array<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(0) @binding(3) var normal_array: texture_2d_array<f32>;   // GL-convention
@group(0) @binding(4) var arm_array: texture_2d_array<f32>;      // R=AO G=rough B=metal
@group(1) @binding(0) var<uniform> object: TerrainUniforms;

// Size of one texture tile in world meters. Structural, so compile-time: it is
// a property of how the terrain materials are authored, not a runtime knob.
// Smaller = denser texels (2.5 m over a 1k pack ~= 410 px/m). Tighter tiling
// also repeats more visibly; killing that repetition is a separate concern
// (stochastic/hex tiling — the V1 "sophisticated terrain blending" item).
const TERRAIN_MATERIAL_SCALE: f32 = 2.5;  // meters per texture tile

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) layer_indices: vec4<u32>,   // Uint8x4 -> components in 0..255
    @location(3) blend_weights: vec4<f32>,   // Unorm8x4 -> components in 0..1
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldNormal: vec3<f32>,
    @location(1) worldOffsetPos: vec3<f32>,
    @location(2) w0: vec4<f32>,   // interpolated weights for layers 0..3
    @location(3) w1: vec4<f32>,   // interpolated weights for layers 4..7
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    // modelMatrix is already rebased into camera-offset space on the CPU.
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldOffsetPos = world_offset_pos;

    // Scatter the (layer, weight) pairs into 8 per-layer weight slots.
    var w = array<f32, 8>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    for (var k = 0u; k < 4u; k = k + 1u) {
        let li = min(input.layer_indices[k], 7u);
        w[li] = w[li] + input.blend_weights[k];
    }
    output.w0 = vec4<f32>(w[0], w[1], w[2], w[3]);
    output.w1 = vec4<f32>(w[4], w[5], w[6], w[7]);
    return output;
}

// Planar UV from world XZ (re-add cameraWorldPos so tiling is world-stable).
fn planar_uv(world_offset_pos: vec3<f32>) -> vec2<f32> {
    let world = world_offset_pos + frame.cameraWorldPos.xyz;
    return world.xz / TERRAIN_MATERIAL_SCALE;
}

// Tangent frame for the planar XZ projection above: dP/du = +X, dP/dv = +Z.
// B = cross(T, N) yields +Z for flat (+Y) ground — cross(N, T) would give -Z
// and invert the normal map's green channel.
//
// Seeding from +X is degenerate when N is (anti)parallel to +X (a vertical
// face); fall back to +Z there purely to avoid a NaN. The planar projection is
// badly stretched on such faces anyway — triplanar is the real fix (deferred).
fn planar_tbn(n: vec3<f32>) -> mat3x3<f32> {
    let N = normalize(n);
    var seed = vec3<f32>(1.0, 0.0, 0.0);
    if (abs(N.x) > 0.99) {
        seed = vec3<f32>(0.0, 0.0, 1.0);
    }
    let T = normalize(seed - N * dot(seed, N));
    let B = cross(T, N);
    return mat3x3<f32>(T, B, N);  // columns: worldN = T*tn.x + B*tn.y + N*tn.z
}

struct BlendedSurface {
    albedo: vec3<f32>,
    tangent_normal: vec3<f32>,
    ao: f32,
    roughness: f32,
}

// Blend the array layers by their interpolated weights; normalized.
//
// Zero-weight layers are SKIPPED, which matters: at 8 slots x 3 arrays a naive
// unconditional loop costs 24 samples/fragment, while a real fragment usually
// blends <=2 layers (~6 samples). The skip is why this takes explicit gradients:
// `textureSample` inside non-uniform control flow is ILLEGAL (implicit
// derivatives need uniform control flow), so dx/dy are computed once by the
// caller (uniform) and fed to textureSampleGrad here.
//
// The array index is clamped to the bound arrays' real layer count, so an 8-slot
// weight vector is safe against a smaller array (e.g. one layer per biome).
fn sample_blended(uv: vec2<f32>, w0: vec4<f32>, w1: vec4<f32>,
                  dx: vec2<f32>, dy: vec2<f32>) -> BlendedSurface {
    var albedo = vec3<f32>(0.0);
    var tn = vec3<f32>(0.0);
    var arm = vec2<f32>(0.0);   // (ao, roughness)
    var wsum = 0.0;
    let max_layer = textureNumLayers(albedo_array) - 1u;

    for (var i = 0u; i < 8u; i = i + 1u) {
        var w = 0.0;
        if (i < 4u) {
            w = w0[i];
        } else {
            w = w1[i - 4u];
        }
        if (w > 0.0) {
            let li = min(i, max_layer);
            albedo = albedo + w * textureSampleGrad(albedo_array, terrain_sampler, uv, li, dx, dy).rgb;
            let n = textureSampleGrad(normal_array, terrain_sampler, uv, li, dx, dy).rgb * 2.0 - 1.0;
            tn = tn + w * n;
            let a = textureSampleGrad(arm_array, terrain_sampler, uv, li, dx, dy);
            arm = arm + w * vec2<f32>(a.r, a.g);
            wsum = wsum + w;
        }
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

@fragment
fn fs_gbuffer(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    let uv = planar_uv(input.worldOffsetPos);
    // Derivatives MUST be taken in uniform control flow (before sample_blended's
    // per-layer branches) — see sample_blended.
    let dx = dpdx(uv);
    let dy = dpdy(uv);

    let surf = sample_blended(uv, input.w0, input.w1, dx, dy);

    let TBN = planar_tbn(input.worldNormal);
    output.normals = encodeOctahedron(normalize(TBN * surf.tangent_normal));
    output.albedo = vec4<f32>(surf.albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = vec4<f32>(surf.roughness, 1.0, surf.ao, 0.0);
    return output;
}
