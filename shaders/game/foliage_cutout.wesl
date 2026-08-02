// Alpha-cutout leaf CARDS, rendered deferred (volumetric-foliage LOD4 work,
// step 5).
//
// The card path was the last piece of foliage still going through the forward
// pass (MaterialLibrary::TranslucentFoliage + AddForwardOpaqueMeshEntity),
// which meant the finest level of the chain was lit differently from every
// coarser one: no GTAO, no contact shadows, and a separate lighting path to
// keep in step. A card is opaque wherever it is opaque -- the only thing it
// needs beyond `voxel_foliage` is an alpha test -- so there is no reason for it
// to be forward.
//
// Deliberately a near-copy of voxel_foliage.wesl's non-instanced variant, plus
// a texture and a discard. Same shading model (kShadingModelFoliage), same
// group-1 per-object UBO layout, so the two levels cannot drift in how they
// light.
//
// Two variants:
//   * gbuffer -- packVoxelFoliageGBuffer, alpha-tested.
//   * shadow  -- depth only, but WITH a fragment stage, because a cutout that
//     applies to the colour pass and not the shadow pass casts the shadow of a
//     solid quad. Same requirement the impostor has.

@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, packVoxelFoliageGBuffer};

// Group-0 bindings start at 1: binding 0 is the engine's frame UBO.
@group(0) @binding(1) var leaf_texture: texture_2d<f32>;
@group(0) @binding(2) var leaf_sampler: sampler;

struct FoliageCutoutUniforms {
    modelMatrix: mat4x4<f32>,
    tint: vec4<f32>,    // rgb = albedo tint
    // x = roughness, y = translucency strength, z = alpha cutoff, w = AO.
    params: vec4<f32>,
}
@group(1) @binding(0) var<uniform> object: FoliageCutoutUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,  // unused: no TBN, no normal map
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
}

fn buildVertex(input: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    out.position = vec4<f32>(world, 1.0);  // projected by each vs_main
    out.uv = input.uv;
    // Uniform-scale assumption, as everywhere else in this family.
    out.worldNormal =
        normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    return out;
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var out = buildVertex(input);
    out.position = worldCameraOffsetedSpaceToClipSpace(out.position.xyz);
    return out;
}

@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var out = buildVertex(input);
    out.position = worldCameraOffsetedSpaceToLightClipSpace(out.position.xyz);
    return out;
}

@if(!shadow_pass)
@fragment
fn fs_main(in: VertexOutput) -> GBufferOutput {
    let texel = textureSample(leaf_texture, leaf_sampler, in.uv);
    if (texel.a < object.params.z) {
        discard;
    }
    // Two-sided: a card is seen from either face, and a back-facing normal
    // would light half the crown inside out. Flipped toward the camera, the
    // same thing the impostor bake does against its own view direction.
    var n = normalize(in.worldNormal);
    // modelMatrix produces CAMERA-OFFSET space (this shader projects with
    // worldCameraOffsetedSpaceToClipSpace), so the camera is at the origin of
    // that space and the vector toward it is simply the negated object
    // position. Subtracting from frame.cameraWorldPos instead mixes the two
    // spaces and evaluates to 2*camWorld - objWorld -- which points the right
    // way ONLY while the object sits at the world origin, i.e. exactly the
    // model viewer's tree, so it would have gone unnoticed until a card mesh
    // was placed anywhere else and half its crown lit inside out.
    let to_eye = normalize(
        -(object.modelMatrix * vec4<f32>(0.0, 0.0, 0.0, 1.0)).xyz);
    if (dot(n, to_eye) < 0.0) {
        n = -n;
    }
    return packVoxelFoliageGBuffer(n, texel.rgb * object.tint.rgb,
                                   object.params.x, object.params.w,
                                   object.params.y);
}

@if(shadow_pass)
@fragment
fn fs_main(in: VertexOutput) {
    let texel = textureSample(leaf_texture, leaf_sampler, in.uv);
    if (texel.a < object.params.z) {
        discard;
    }
}
