// Instanced deferred (G-buffer) PBR material. Forked from normalmapped.wesl
// (2D mode) for the GPU-driven instanced-rendering feature: the fragment stage
// is copied verbatim (TBN + octahedron-encoded normals -> GBufferOutput), and
// only the vertex stage + group layout change. The per-object model matrix is
// read per-instance from a group-1 storage array indexed by
// @builtin(instance_index); material constants (a tint) live in a group-0 UBO.
//
// Feature: `instanced` (pushed by MeshRenderingMaterial::BuildDeclaration for
// GeometryType::kInstancedMesh). Vertex layout: the standard textured-mesh
// stream pos(3)+uv(2)+normal(3)+tangent(3) — instances are NOT a vertex stream.
//
// Textures (group 0): albedo (RGB), normal (tangent-space, GL convention),
// ARM (R=AO, G=roughness, B=metallic unused) — identical to normalmapped.

@if(!shadow_pass)
import package::common::frame::{frame, worldSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldSpaceToWorldCameraOffsetedSpace, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};

// Group 0: frame UBO (@0, imported) + material textures/sampler + material
// constants UBO (@5). The model matrix is NOT here — it is per-instance.
@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var material_sampler: sampler;
@group(0) @binding(3) var normal_texture: texture_2d<f32>;
@group(0) @binding(4) var arm_texture: texture_2d<f32>;

struct MaterialParams {
    tint: vec4<f32>,   // rgb = albedo tint, a unused
}
@group(0) @binding(5) var<uniform> mat_params: MaterialParams;

// Group 1: per-instance world transforms, indexed by @builtin(instance_index).
@group(1) @binding(0) var<storage, read> instances: array<mat4x4<f32>>;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

@if(!shadow_pass)
struct VertexOutputGBuffer {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) worldTangent: vec3<f32>,
}

@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;

    let m = instances[instance_index];
    // Instance matrices hold FULL world transforms; worldSpaceToClipSpace
    // applies the camera-offset internally (proj * view * (world - cameraPos)).
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldSpaceToClipSpace(world);

    // Uniform-scale assumption (fine for instanced foliage): use m directly for
    // normal/tangent rather than the inverse-transpose.
    output.worldNormal = normalize((m * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldTangent = normalize((m * vec4<f32>(input.tangent, 0.0)).xyz);
    output.uv = input.uv;

    return output;
}

@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    let m = instances[instance_index];
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(
        worldSpaceToWorldCameraOffsetedSpace(world));
    return output;
}

@if(!shadow_pass)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    var output: GBufferOutput;

    // Reconstruct TBN matrix from interpolated vectors
    let N = normalize(input.worldNormal);
    let T = normalize(input.worldTangent - N * dot(input.worldTangent, N)); // Gram-Schmidt
    let B = cross(N, T);
    let TBN = mat3x3<f32>(T, B, N);

    // Sample textures
    let albedo = textureSample(albedo_texture, material_sampler, input.uv).rgb * mat_params.tint.rgb;
    let normalMap = textureSample(normal_texture, material_sampler, input.uv).rgb;
    let arm = textureSample(arm_texture, material_sampler, input.uv);
    let roughness = arm.g;
    let ao = arm.r;

    let tangentNormal = normalMap * 2.0 - 1.0;
    let worldNormal = normalize(TBN * tangentNormal);

    output.normals = encodeOctahedron(worldNormal);
    output.albedo = vec4<f32>(albedo, 1.0);
    // Material: R=roughness, G=shadow (1.0 = no shadow), B=baked AO, A=reserved
    output.material = vec4<f32>(roughness, 1.0, ao, 0.0);

    return output;
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    // Depth is written automatically via @builtin(position)
}
