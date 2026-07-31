// Textureless deferred (G-buffer) material for voxelized foliage (leaf-card
// -> tet-mesh voxelization, Phase 1 of the volumetric-foliage feature —
// src/game/geometry/leaf_voxelizer.hpp). No albedo/normal/ARM textures: each
// tet vertex's uv.x carries a baked per-voxel brightness (0..1), tinted by
// the material's uniform tint, so the whole shading model needs no bound
// texture or sampler (a sampler with no texture left unbound is a Dawn
// validation error -- this material declares neither). Writes the foliage
// shading model via packVoxelFoliageGBuffer (gbuffer_encode.wesl): material.a
// = kShadingModelFoliage so deferred_lighting.wesl adds a transmission term
// for these pixels -- Phase 2 only wires the encode + the deferred branch;
// nothing attaches this material to real geometry yet (Phase 3+).
//
// Two orthogonal features, 4 compiled variants total (both pushed
// automatically by MeshRenderingMaterial::BuildDeclaration for the geometry/
// pass combo -- see material.cpp:75-80):
//   - `instanced`: GeometryType::kInstancedMesh -- per-instance transform
//     read from a group-1 storage array (instanced_gbuffer.wesl's scheme:
//     group-0 MaterialParams UBO + group-1 instances/bucketBase).
//   - `!instanced`: GeometryType::kTexturedMesh -- a group-1 per-object UBO
//     (standard_forward.wesl's scheme: modelMatrix + tint + params).
//   - `shadow_pass`: RenderPassType::kShadow -- depth-only, no fragment
//     output (opaque, no alpha test: voxel leaves are solid tets).
//
// Vertex layout: the standard textured-mesh stream pos(3)+uv(2)+normal(3)+
// tangent(3) -- tangent is unused (no TBN, no normal map); for `instanced`,
// instances are NOT a vertex stream (matches instanced_gbuffer.wesl).

@if(!instanced && !shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(!instanced && shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(instanced && !shadow_pass)
import package::common::frame::{frame, worldSpaceToClipSpace};
@if(instanced && shadow_pass)
import package::common::frame::{frame, worldSpaceToWorldCameraOffsetedSpace, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, packVoxelFoliageGBuffer};

// Non-instanced: group-1 per-object UBO (modelMatrix + material constants),
// modeled on standard_forward.wesl's StandardForwardUniforms. `params`:
// x = roughness, y = translucency strength.
@if(!instanced)
struct VoxelFoliageUniforms {
    modelMatrix: mat4x4<f32>,
    tint: vec4<f32>,     // rgb = albedo tint
    params: vec4<f32>,   // x = roughness, y = translucency strength
}
@if(!instanced)
@group(1) @binding(0) var<uniform> object: VoxelFoliageUniforms;

// Instanced: group-0 material-constants UBO (modeled on instanced_gbuffer
// .wesl's MaterialParams, with the same roughness/strength `params` field
// standard_forward.wesl uses) + group-1 the whole compacted per-(model,lod)
// transform buffer + GPU-computed per-bucket base offsets -- see
// instanced_gbuffer.wesl's file comment for the bucketBase[bucketId] +
// instance_index recovery scheme this vertex stage reuses verbatim.
@if(instanced)
struct MaterialParams {
    tint: vec4<f32>,     // rgb = albedo tint
    params: vec4<f32>,   // x = roughness, y = translucency strength
    bucketId: u32,       // which (model,lod) bucket THIS draw renders
}
@if(instanced)
@group(0) @binding(1) var<uniform> mat_params: MaterialParams;
@if(instanced)
@group(1) @binding(0) var<storage, read> instances: array<mat4x4<f32>>;
@if(instanced)
@group(1) @binding(1) var<storage, read> bucketBase: array<u32>;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,  // unused -- no TBN, no normal map
}

@if(!shadow_pass)
struct VertexOutputGBuffer {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
}

@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

@if(!instanced && !shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;
    // object.modelMatrix is already rebased to WorldCameraOffsetedSpace by
    // the caller (render_textured_mesh.cpp), matching normalmapped.wesl /
    // standard_forward.wesl's non-instanced convention.
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world);
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.uv = input.uv;
    return output;
}

@if(!instanced && shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world);
    return output;
}

@if(instanced && !shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;
    let base = bucketBase[mat_params.bucketId];
    let m = instances[base + instance_index];
    // Instance matrices hold FULL world transforms (unlike the non-instanced
    // object UBO above); worldSpaceToClipSpace applies the camera offset
    // internally -- see instanced_gbuffer.wesl's file comment.
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldSpaceToClipSpace(world);
    output.worldNormal = normalize((m * vec4<f32>(input.normal, 0.0)).xyz);
    output.uv = input.uv;
    return output;
}

@if(instanced && shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    let base = bucketBase[mat_params.bucketId];
    let m = instances[base + instance_index];
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(
        worldSpaceToWorldCameraOffsetedSpace(world));
    return output;
}

@if(!shadow_pass && !instanced)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    // No texture: albedo is the vertex-baked brightness (uv.x, see
    // leaf_voxelizer.hpp's tet vertex layout) tinted by the material's tint,
    // written RAW (sRGB-domain) -- matching normalmapped/instanced_gbuffer's
    // convention of writing `sample * tint` unlinearized and letting
    // deferred_lighting.wesl's srgb_to_linear(albedo_texture) do the one and
    // only linearization on READ. Linearizing here too would double it.
    let albedo = vec3<f32>(input.uv.x) * object.tint.rgb;
    return packVoxelFoliageGBuffer(input.worldNormal, albedo,
                                   object.params.x, 1.0, object.params.y);
}

@if(!shadow_pass && instanced)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    // Same RAW sRGB-domain convention as the non-instanced fs_main above.
    let albedo = vec3<f32>(input.uv.x) * mat_params.tint.rgb;
    return packVoxelFoliageGBuffer(input.worldNormal, albedo,
                                   mat_params.params.x, 1.0, mat_params.params.y);
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    // Depth is written automatically via @builtin(position). Opaque, no
    // alpha test -- the point of a textureless voxel material.
}
