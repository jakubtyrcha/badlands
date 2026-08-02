// The volumetric billboard that replaces a tree's geometry at LOD4.
//
// "Volumetric" as opposed to a flat unlit card: it writes albedo, a real world
// NORMAL and a translucency strength into the G-buffer via
// packVoxelFoliageGBuffer, so the deferred pass lights it -- including the
// back-lit transmission term -- exactly as it lights the voxel crowns it takes
// over from. No new shading model: kShadingModelFoliage already existed.
//
// The quad is built entirely in the vertex stage from the instance transform;
// the vertex buffer is a bare 4-vertex unit quad and carries no position that
// matters. Its uv IS the local uv into the baked tile.
//
// TWO variants, and the difference is which direction it faces:
//   * gbuffer -- faces the CAMERA, samples the octahedral view for the camera.
//   * shadow  -- faces the LIGHT, samples the octahedral view for the light.
// The second is the whole reason this is octahedral rather than a yaw ring.
// GpuInstanceRenderer::CullShadow selects LOD from the CAMERA's position (so
// shadow geometry matches what the camera draws), which means an L4 instance
// casts its shadow FROM THIS QUAD. A camera-facing quad lit from the side casts
// a sliver; facing the light and sampling the light's own view casts a
// correctly tree-shaped one, off the same atlas.
//
// Sampling uses an EXPLICIT LOD, not derivatives. The atlas packs 16 views per
// layer, so the legal uv range for a tile depends on the mip (see
// impostor_atlas.hpp's texel-centre mapping) -- a derivative-chosen mip could
// not be inset for. Derivatives are unreliable at an alpha-cutout silhouette
// anyway.

@if(!shadow_pass && instanced)
import package::common::frame::{frame, worldSpaceToClipSpace};
@if(!shadow_pass && !instanced)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass && instanced)
import package::common::frame::{frame, worldSpaceToWorldCameraOffsetedSpace, worldCameraOffsetedSpaceToLightClipSpace};
@if(shadow_pass && !instanced)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, packVoxelFoliageGBuffer};
import package::common::frame::decodeOctahedron;

// Must match src/game/visual/octahedral.hpp and impostor_atlas.hpp. These are
// compile-time constants on both sides; game/tests/octahedral_tests.cpp and
// impostor_atlas_tests.cpp pin the C++ half.
const kViewsPerAxis: f32 = 4.0;
const kTilePx: f32 = 128.0;
const kLayerPx: f32 = 512.0;
const kMipLevels: f32 = 6.0;

// How the baked thickness (0..1 over the impostor's own diameter) maps onto the
// two terms it drives. Both are shaping constants, not physics: the bake's
// thickness is already normalized by a per-model radius, so a coefficient here
// is the only place a "how dense is a crown" judgement can live.
//
// The AO pair is set against WHAT THE VOXEL LEVEL GETS, not against taste. A
// voxel crown writes a baked AO of 1.0 (voxel_foliage.wesl) because it has real
// geometry, so the deferred pass's screen-space GTAO can occlude its interior
// for it. A two-triangle impostor gives GTAO nothing to work with, so this term
// stands IN for that -- it should approximate the occlusion the voxel crown
// already receives, not add more on top of it. The first values here (1.6 with
// a 0.25 floor) did the latter and made the impostor visibly darker than the
// level it replaces, worst on sparse crowns where a mid-crown pixel sat on the
// floor at 0.25 against the voxel level's 1.0 -- a 4x step at the LOD switch.
const kThicknessToOpacity: f32 = 6.0;  // transmission falloff with depth
const kThicknessToAo: f32 = 0.5;       // interior occlusion
const kMinAo: f32 = 0.5;               // never darker than GTAO would go

// The material's own constants. WHERE they live is fixed by an engine contract
// (material_instance.cpp's SelectMaterialParamsBuffer): an INSTANCED material
// keeps them in a group-0 UBO, because group 1 is already the per-object
// storage array; every other geometry keeps them in the group-1 per-object UBO.
// Declaring a group-0 params UBO on the non-instanced variant leaves it unbound
// -- the factory looks in group 1, finds the object UBO, and never binds the
// other -- which surfaces as a bind-group entry-count mismatch at draw time.
//
// `params.w` carries the atlas LAYER (the model id) as a float rather than a
// separate u32 member, so both variants have the identical field list and one
// accessor pair can serve them.
@if(instanced)
struct MaterialParams {
    // xyz = the model's placement centre in LOCAL units, w = its frame radius.
    placement: vec4<f32>,
    // x = roughness, y = translucency strength, z = alpha cutoff, w = layer.
    params: vec4<f32>,
    // Which (model, lod) bucket this draw renders.
    bucketId: u32,
}

// Group-0 bindings start at 1: binding 0 is the engine's FRAME UBO
// (shaders/common/frame.wesl), which every material shader importing `frame`
// inherits. instanced_gbuffer.wesl leaves it free for the same reason. Taking
// binding 0 here produced a duplicate-binding bind-group-layout error, not a
// compile error.
@group(0) @binding(1) var impostor_albedo: texture_2d_array<f32>;
@group(0) @binding(2) var impostor_surface: texture_2d_array<f32>;
@group(0) @binding(3) var impostor_sampler: sampler;
@if(instanced)
@group(0) @binding(4) var<uniform> mat_params: MaterialParams;

// Group 1 is where the two GEOMETRY variants differ, and only there.
//   instanced  (kInstancedMesh) -- the compacted per-(model,lod) transform
//                buffer plus the GPU-computed per-bucket base offsets.
//   !instanced (kTexturedMesh)  -- one per-object UBO, the standard single-mesh
//                scheme. This exists so the model viewer can show an impostor
//                as a single previewable object in the same slot the retired
//                voxel L3 used to occupy.
@if(instanced)
@group(1) @binding(0) var<storage, read> instances: array<mat4x4<f32>>;
@if(instanced)
@group(1) @binding(1) var<storage, read> bucketBase: array<u32>;

@if(!instanced)
struct ObjectUniforms {
    modelMatrix: mat4x4<f32>,
    placement: vec4<f32>,
    params: vec4<f32>,
}
@if(!instanced)
@group(1) @binding(0) var<uniform> object: ObjectUniforms;

// The constants, wherever this variant keeps them.
// The camera, in whatever space this variant's matrices produce. Functions, not
// `@if`-ed statements: WESL gates DECLARATIONS only.
@if(instanced)
fn eyePos() -> vec3<f32> { return frame.cameraWorldPos.xyz; }
// Camera-offset space puts the camera at the origin by construction.
@if(!instanced)
fn eyePos() -> vec3<f32> { return vec3<f32>(0.0); }

@if(!shadow_pass && instanced)
fn projectQuad(p: vec3<f32>) -> vec4<f32> { return worldSpaceToClipSpace(p); }
@if(!shadow_pass && !instanced)
fn projectQuad(p: vec3<f32>) -> vec4<f32> {
    return worldCameraOffsetedSpaceToClipSpace(p);
}
@if(shadow_pass && instanced)
fn projectQuad(p: vec3<f32>) -> vec4<f32> {
    return worldCameraOffsetedSpaceToLightClipSpace(
        worldSpaceToWorldCameraOffsetedSpace(p));
}
@if(shadow_pass && !instanced)
fn projectQuad(p: vec3<f32>) -> vec4<f32> {
    return worldCameraOffsetedSpaceToLightClipSpace(p);
}

@if(instanced)
fn matPlacement() -> vec4<f32> { return mat_params.placement; }
@if(!instanced)
fn matPlacement() -> vec4<f32> { return object.placement; }
@if(instanced)
fn matParams() -> vec4<f32> { return mat_params.params; }
@if(!instanced)
fn matParams() -> vec4<f32> { return object.params; }

// The instance transform, however this variant gets one. Everything downstream
// is identical, which is why the split stops here rather than forking the
// vertex stages.
@if(instanced)
fn instanceMatrix(idx: u32) -> mat4x4<f32> {
    return instances[bucketBase[mat_params.bucketId] + idx];
}
@if(!instanced)
fn instanceMatrix(idx: u32) -> mat4x4<f32> {
    return object.modelMatrix;
}

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) local_uv: vec2<f32>,
    // The direction from the tree toward the eye, in the TREE'S OWN space --
    // i.e. with the instance's yaw undone. This is what selects the views.
    @location(1) view_local: vec3<f32>,
    // The INSTANCE'S ROTATION, column by column.
    //
    // Not the quad's basis, which is what this used to carry and was wrong: a
    // baked normal is stored in the tree's own CARTESIAN space (impostor_bake
    // .wesl writes the vertex normal untransformed), so turning it into a world
    // normal is that rotation applied to it -- NOT a reinterpretation of its
    // components as coordinates in the quad's right/up/toEye frame. Under the
    // old reading a stationary tree's shading swung as the camera orbited, and
    // the gbuffer and shadow variants disagreed about the same texel because
    // they build different frames (camera-facing vs light-facing).
    @location(2) rot0: vec3<f32>,
    @location(3) rot1: vec3<f32>,
    @location(4) rot2: vec3<f32>,
    @location(5) mip: f32,
    // The quad's own position, and the axis + half-extent needed to walk from it
    // into the volume. Together these let the fragment stage turn the baked
    // depth into a real position, and so into a real frag_depth.
    @location(6) quad_pos: vec3<f32>,
    @location(7) to_eye_radius: vec4<f32>,  // xyz = toward the eye, w = radius
}

// The G-buffer, plus a depth the fragment stage computes.
//
// A local copy of GBufferOutput rather than a change to it: adding
// @builtin(frag_depth) to the shared struct would put a depth write on every
// material that uses it. Field layout must stay in step with
// gbuffer_encode.wesl.
@if(!shadow_pass)
struct ImpostorGBufferOutput {
    @location(0) normals: vec2<f32>,
    @location(1) albedo: vec4<f32>,
    @location(2) material: vec4<f32>,
    @builtin(frag_depth) depth: f32,
}

// The bake's up rule (impostor_baker.cpp's BakeView), mirrored exactly. A quad
// built on a different basis than the tile was baked with rolls the tree about
// its own view axis.
fn stableUp(dir: vec3<f32>) -> vec3<f32> {
    if (abs(dir.y) > 0.99) {
        return vec3<f32>(0.0, 0.0, 1.0);
    }
    return vec3<f32>(0.0, 1.0, 0.0);
}

// Everything both variants share. Returns `position` holding the WORLD-space
// quad corner in xyz; each vs_main projects it, because the two go to different
// clip spaces.
//
// Split this way because WESL's `@if` gates DECLARATIONS, not statements -- a
// single vs_main with `@if`-ed lines inside it does not parse. Same shape as
// voxel_foliage.wesl's two vertex stages.
// `eye` is the camera's position IN THE SAME SPACE `m` produces. The two
// geometry variants do not agree on that space and it is not optional: an
// INSTANCED matrix is a full world transform, while a non-instanced per-object
// modelMatrix produces CAMERA-OFFSET space (which is why voxel_foliage's
// non-instanced path projects with worldCameraOffsetedSpaceToClipSpace). Mixing
// them puts the quad's centre and the camera in different spaces, so `to_eye`
// is garbage and the projection double-applies the offset -- the quad simply
// lands somewhere off-screen and the level renders as nothing.
fn buildQuadVertex(m: mat4x4<f32>, uv: vec2<f32>, eye: vec3<f32>,
                   to_eye_override: vec3<f32>, use_override: bool)
    -> VertexOutput {
    var out: VertexOutput;

    // The instance transform is translate * rotateY * translate * uniformScale,
    // so its upper 3x3 is R*s with s recoverable from any column's length.
    let scale = length(m[0].xyz);
    let center = (m * vec4<f32>(matPlacement().xyz, 1.0)).xyz;
    let radius = matPlacement().w * scale;

    let to_eye = select(normalize(eye - center), to_eye_override, use_override);
    let up0 = stableUp(to_eye);
    let right = normalize(cross(up0, to_eye));
    let up = cross(to_eye, right);

    // uv.y = 0 is the TOP of a baked tile (view-space +y maps to +NDC.y maps to
    // v = 0), so v grows downward along -up.
    // The quad stays on the bounding sphere's CENTRE plane.
    //
    // The near TANGENT plane is the placement conservative depth would want --
    // every real surface is then behind the rasterized depth, so the shader
    // only ever moves depth one way (`less` under reversed-Z) and early-Z
    // survives. It was tried and reverted: the bake is ORTHOGRAPHIC, so a quad
    // pulled a full radius toward the camera magnifies the baked image under
    // perspective (half-width r at distance d-r subtends more than a sphere of
    // radius r at d). The error vanishes as d/r grows and is invisible at LOD4
    // range, but it is a real silhouette error and it buys nothing while the
    // WESL front end cannot state the conservative-depth promise (see the
    // plan). Centre plane keeps the silhouette exact; the frag_depth write
    // below is what recovers the volume either way.
    let offset = right * (uv.x * 2.0 - 1.0) * radius +
                 up * (1.0 - uv.y * 2.0) * radius;
    let quad_pos = center + offset;
    out.position = vec4<f32>(quad_pos, 1.0);
    out.quad_pos = quad_pos;
    out.to_eye_radius = vec4<f32>(to_eye, radius);

    out.local_uv = uv;
    out.rot0 = m[0].xyz / scale;
    out.rot1 = m[1].xyz / scale;
    out.rot2 = m[2].xyz / scale;

    // Undo the instance's rotation to get the view direction in the tree's own
    // space. The upper 3x3 is R*s with R orthonormal, so the inverse rotation
    // is its transpose, and dividing by s makes the columns unit length.
    let inv_r = transpose(mat3x3<f32>(m[0].xyz / scale, m[1].xyz / scale,
                                      m[2].xyz / scale));
    out.view_local = normalize(inv_r * to_eye);

    // Explicit LOD from the quad's on-screen size. Always measured against the
    // CAMERA, in both variants: a shadow's importance tracks how big the caster
    // is on screen, and it keeps the two variants sampling the same level.
    let dist = max(length(eye - center), 0.001);
    let focal_px = 0.5 * frame.screenSize.y * frame.projMatrix[1][1];
    let screen_px = max(2.0 * radius / dist * focal_px, 1.0);
    // ROUNDED to an integer level, and that is load-bearing. The atlas sampler
    // uses MipmapFilterMode::Nearest, so textureSampleLevel fetches round(mip)
    // -- while tileUv would inset for the FRACTIONAL mip. At mip = k + 0.5 the
    // inset is only 0.354 texels of the level actually sampled, inside the
    // half-texel guard band, and the bilinear tap reaches the neighbouring
    // view's tile. Rounding here makes the inset and the fetch agree on one
    // level, which is what makes the no-bleed claim true.
    out.mip = round(clamp(log2(kTilePx / screen_px), 0.0, kMipLevels - 1.0));

    return out;
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32)
    -> VertexOutput {
    let m = instanceMatrix(instance_index);
    var out = buildQuadVertex(m, input.uv, eyePos(), vec3<f32>(0.0), false);
    out.position = projectQuad(out.position.xyz);
    return out;
}

// The shadow variant faces the LIGHT. The sun is directional, so "toward the
// eye" is simply the sun direction (which points TOWARD the sun by
// SceneContext convention) and does not depend on where the tree stands.
@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32)
    -> VertexOutput {
    let m = instanceMatrix(instance_index);
    var out = buildQuadVertex(m, input.uv, eyePos(),
                              normalize(frame.sunDirection.xyz), true);
    out.position = projectQuad(out.position.xyz);
    return out;
}

// --- The octahedral lookup, mirroring src/game/visual/octahedral.hpp ---

fn hemiOctEncode(d_in: vec3<f32>) -> vec2<f32> {
    var d = d_in;
    d.y = max(d.y, 0.0);
    let l1 = abs(d.x) + abs(d.y) + abs(d.z);
    if (!(l1 > 0.0)) {
        return vec2<f32>(0.5);
    }
    let n = d / l1;
    return vec2<f32>(n.x + n.z, n.z - n.x) * 0.5 + 0.5;
}

// Layer-space uv for `local_uv` within view (i, j) at `mip`, onto the tile's
// TEXEL CENTRES -- impostor_atlas.hpp's ImpostorTileUv, exactly.
fn tileUv(i: f32, j: f32, local_uv: vec2<f32>, mip: f32) -> vec2<f32> {
    let shrink = exp2(mip);
    let tile = kTilePx / shrink;
    let layer = kLayerPx / shrink;
    let origin = vec2<f32>(i, j) * tile;
    let texel = origin + 0.5 + local_uv * (tile - 1.0);
    return texel / layer;
}

struct Sampled {
    albedo: vec4<f32>,
    surface: vec4<f32>,
}

fn sampleView(i: f32, j: f32, local_uv: vec2<f32>, mip: f32,
              layer: u32) -> Sampled {
    let uv = tileUv(i, j, local_uv, mip);
    var s: Sampled;
    s.albedo = textureSampleLevel(impostor_albedo, impostor_sampler, uv, layer,
                                  mip);
    s.surface = textureSampleLevel(impostor_surface, impostor_sampler, uv, layer,
                                   mip);
    return s;
}

// The three baked views for `view_local`, blended barycentrically -- the
// containing triangle of the quad of tile centres, NOT the three nearest views
// (see ImpostorBlendFor: nearest-3 flips discontinuously and pops).
fn blendViews(view_local: vec3<f32>, local_uv: vec2<f32>, mip: f32,
              layer: u32) -> Sampled {
    let uv = hemiOctEncode(view_local);
    let g = uv * kViewsPerAxis - 0.5;
    let i0 = clamp(floor(g.x), 0.0, kViewsPerAxis - 2.0);
    let j0 = clamp(floor(g.y), 0.0, kViewsPerAxis - 2.0);
    let f = clamp(g - vec2<f32>(i0, j0), vec2<f32>(0.0), vec2<f32>(1.0));

    var s0: Sampled;
    var s1: Sampled;
    var s2: Sampled;
    var w = vec3<f32>(0.0);
    if (f.x + f.y <= 1.0) {
        s0 = sampleView(i0, j0, local_uv, mip, layer);
        s1 = sampleView(i0 + 1.0, j0, local_uv, mip, layer);
        s2 = sampleView(i0, j0 + 1.0, local_uv, mip, layer);
        w = vec3<f32>(1.0 - f.x - f.y, f.x, f.y);
    } else {
        s0 = sampleView(i0 + 1.0, j0 + 1.0, local_uv, mip, layer);
        s1 = sampleView(i0 + 1.0, j0, local_uv, mip, layer);
        s2 = sampleView(i0, j0 + 1.0, local_uv, mip, layer);
        w = vec3<f32>(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
    }

    // PREMULTIPLIED by coverage, not merely by the barycentric weight.
    //
    // The three views disagree about coverage wherever the silhouette differs
    // between them -- which for a stippled crown is most of it. A plain
    // sum(texel * weight) then drags a texel that is covered in ONE view toward
    // the empty views' clear value: its colour comes out scaled by that view's
    // weight alone while its alpha can still clear the cutoff, so it renders
    // dark. The whole impostor reads darker than the voxel level it replaces,
    // worst on the sparsest crowns.
    //
    // Weighting colour by each view's own alpha and dividing by the total is
    // the same correction the mip downsample already makes (see
    // impostor_baker.cpp's DownsamplePair) -- it was simply never applied here.
    // The SURFACE map is weighted by the ALBEDO's alpha for the same reason:
    // its own alpha is thickness, not coverage, and an empty view would
    // otherwise pull the encoded normal toward the (0.5, 0.5) clear.
    let cw = w * vec3<f32>(s0.albedo.a, s1.albedo.a, s2.albedo.a);
    let total = cw.x + cw.y + cw.z;
    let inv = 1.0 / max(total, 1e-4);

    var out: Sampled;
    out.albedo = vec4<f32>(
        (s0.albedo.rgb * cw.x + s1.albedo.rgb * cw.y + s2.albedo.rgb * cw.z) * inv,
        total);
    out.surface =
        (s0.surface * cw.x + s1.surface * cw.y + s2.surface * cw.z) * inv;
    return out;
}

// Where the baked depth actually puts this fragment. The bake's depth is 0 at
// the sphere's NEAR tangent plane and 1 at its far one, while the quad sits on
// the CENTRE plane -- so a fragment is offset by radius * (1 - 2 * depth): a
// full radius toward the eye at depth 0, a full radius away at depth 1.
fn surfacePosition(in: VertexOutput, depth: f32) -> vec3<f32> {
    return in.quad_pos +
           in.to_eye_radius.xyz * (in.to_eye_radius.w * (1.0 - 2.0 * depth));
}

// Clip-space z/w for a position, clamped to the valid range. Feeding the depth
// buffer a value outside [0,1] is undefined, and a blended baked depth at a
// silhouette can land slightly outside.
fn depthOf(p: vec3<f32>) -> f32 {
    let clip = projectQuad(p);
    return clamp(clip.z / clip.w, 0.0, 1.0);
}

@if(!shadow_pass)
@fragment
fn fs_main(in: VertexOutput) -> ImpostorGBufferOutput {
    let s = blendViews(in.view_local, in.local_uv, in.mip, u32(matParams().w));
    // Alpha is blended with the same weights and cut AFTER. Cross-fading three
    // cutout silhouettes ghosts slightly at a triangle boundary; at LOD4 range
    // that is sub-pixel, and the alternative -- nearest view only -- pops.
    if (s.albedo.a < matParams().z) {
        discard;
    }

    // The baked normal is in the TREE's own Cartesian space (flipped toward its
    // baked view), so the world normal is just the instance's rotation applied
    // to it.
    let n_local = decodeOctahedron(s.surface.xy * 2.0 - 1.0);
    let n_world = normalize(in.rot0 * n_local.x + in.rot1 * n_local.y +
                            in.rot2 * n_local.z);

    // VOLUME comes from the baked thickness (surface.a): how much crown the
    // view ray actually passes through, summed over the closed tets. Without
    // it the impostor writes a constant AO and a constant translucency, so the
    // whole crown lights as one flat sheet -- which is what a billboard looks
    // like and what the volumetric G-buffer path exists to avoid.
    let thickness = s.surface.w;

    // Thin edges transmit, the dense middle does not. This is the same shape
    // evaluateTranslucency applies with its own exp(-sigma * thicknessWorld);
    // doing it here as well is not double-counting but the part that varies
    // PER PIXEL, since the engine's term uses the shadow-map optical depth,
    // which a two-triangle quad cannot supply.
    let transmit = matParams().y * exp(-kThicknessToOpacity * thickness);

    // ...and thick interior is occluded. A crown's inside sees little sky.
    let ao = clamp(1.0 - kThicknessToAo * thickness, kMinAo, 1.0);

    // Argument order is (normal, albedo, roughness, AO, translucency) -- see
    // gbuffer_encode.wesl.
    let g = packVoxelFoliageGBuffer(n_world, s.albedo.rgb, matParams().x, ao,
                                    transmit);

    var out: ImpostorGBufferOutput;
    out.normals = g.normals;
    out.albedo = g.albedo;
    out.material = g.material;
    // REAL per-pixel depth, which is what buys the volume back: the impostor
    // intersects terrain and its neighbours correctly, and -- because the
    // G-buffer position the deferred pass reconstructs is now the true surface
    // rather than a plane -- the existing shadow map self-shadows it with no
    // change to deferred_lighting.wesl at all.
    out.depth = depthOf(surfacePosition(in, s.surface.z));
    return out;
}

// Depth-only, but WITH a fragment stage: the cutout has to apply to the shadow
// too, or a tree casts the shadow of a solid rectangle. A fragment stage with
// zero color targets is legal and was proven on this Dawn build before the
// plan committed to it.
@if(shadow_pass)
@fragment
fn fs_main(in: VertexOutput) -> @builtin(frag_depth) f32 {
    let s = blendViews(in.view_local, in.local_uv, in.mip, u32(matParams().w));
    if (s.albedo.a < matParams().z) {
        discard;
    }
    // The whole reason the cast shadow was a hard, flat cutout: without this the
    // entire crown casts from one plane, so the shadow is a stamped silhouette
    // with no internal depth. Here the quad faces the LIGHT, so walking the
    // baked depth along that axis gives the caster its real thickness.
    return depthOf(surfacePosition(in, s.surface.z));
}
