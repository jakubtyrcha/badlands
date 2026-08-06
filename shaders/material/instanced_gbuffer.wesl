// Instanced deferred (G-buffer) PBR material. Forked from normalmapped.wesl
// (2D mode) for the GPU-driven instanced-rendering feature: the fragment
// stage's GBuffer assembly (TBN + octahedron-encoded normals -> GBufferOutput)
// is shared with normalmapped.wesl via the common packNormalMappedGBuffer
// helper; only the vertex stage + group layout change. The per-object model
// matrix is read per-instance from a group-1 storage array indexed by
// @builtin(instance_index); material constants (a tint) live in a group-0 UBO.
//
// Feature: `instanced` (pushed by MeshRenderingMaterial::BuildDeclaration for
// GeometryType::kInstancedMesh). Vertex layout: the standard textured-mesh
// stream pos(3)+uv(2)+normal(3)+tangent(4) — instances are NOT a vertex stream.
//
// Textures (group 0): albedo (RGB), normal (tangent-space, GL convention),
// ARM (R=AO, G=roughness, B=metallic unused) — identical to normalmapped.

@if(!shadow_pass)
import package::common::frame::{frame, worldSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldSpaceToWorldCameraOffsetedSpace, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, packNormalMappedGBuffer};

// Group 0: frame UBO (@0, imported) + material textures/sampler + material
// constants UBO (@5). The model matrix is NOT here — it is per-instance.
@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var material_sampler: sampler;
@group(0) @binding(3) var normal_texture: texture_2d<f32>;
@group(0) @binding(4) var arm_texture: texture_2d<f32>;

struct MaterialParams {
    tint: vec4<f32>,   // rgb = albedo tint, a unused
    bucketId: u32,     // which (model,lod) bucket THIS draw renders (Phase D)
}
@group(0) @binding(5) var<uniform> mat_params: MaterialParams;

// Group 1: the WHOLE compacted per-(model,lod) transform buffer (@0) plus the
// GPU-computed per-bucket base offsets (@1). The vertex stage recovers this
// draw's transforms as `instances[bucketBase[mat_params.bucketId] +
// instance_index]` — the CPU knows bucketId per draw but NOT the prefix-summed
// base, so the base is read on the GPU (no per-frame offset readback). Phase C's
// single-bucket path is the trivial case: bucketBase = [0], bucketId = 0.
@group(1) @binding(0) var<storage, read> instances: array<mat4x4<f32>>;
@group(1) @binding(1) var<storage, read> bucketBase: array<u32>;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec4<f32>,
}

@if(!shadow_pass)
struct VertexOutputGBuffer {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) worldTangent: vec4<f32>,
}

@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;

    let base = bucketBase[mat_params.bucketId];
    let m = instances[base + instance_index];
    // Instance matrices hold FULL world transforms; worldSpaceToClipSpace
    // applies the camera-offset internally (proj * view * (world - cameraPos)).
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldSpaceToClipSpace(world);

    // Uniform-scale assumption (fine for instanced meshes): use m directly for
    // normal/tangent rather than the inverse-transpose.
    output.worldNormal = normalize((m * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldTangent = vec4<f32>(
        normalize((m * vec4<f32>(input.tangent.xyz, 0.0)).xyz), input.tangent.w);
    output.uv = input.uv;

    return output;
}

@if(shadow_pass)
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

@if(!shadow_pass)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    // Sample textures
    let albedo = textureSample(albedo_texture, material_sampler, input.uv).rgb * mat_params.tint.rgb;
    let normalMap = textureSample(normal_texture, material_sampler, input.uv).rgb;
    let arm = textureSample(arm_texture, material_sampler, input.uv);

    return packNormalMappedGBuffer(input.worldNormal, input.worldTangent, albedo, normalMap, arm);
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    // Depth is written automatically via @builtin(position)
}
