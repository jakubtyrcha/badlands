// Copied verbatim from sampo's src/shaders/material/normalmapped.wesl (Task
// D1). NOTE: this shader is currently INERT in badlands — it imports
// `package::common::gbuffer_encode` and (for sphere_mode) `package::common::
// eac`, neither of which is ported yet (they belong to the G-buffer/deferred
// rendering pipeline, out of scope for the material-system port). It won't
// compile via GpuPipelineGenerator::GetPipeline until those shared modules
// land. It's kept here so MaterialRequirementsRegistry's "normalmapped"
// entry (material_requirements.cpp) has a corresponding shader file, per the
// D1 task brief. The exercised path for this task is textured_mesh.wesl only.
//
// Normalmapped material shader for textured surfaces
// Outputs to GBuffer for deferred rendering
//
// Supports conditional compilation:
// - sphere_mode: When enabled, uses cubemap sampling
//                When disabled, uses 2D texture sampling
// - shadow_pass: When enabled, outputs depth only (for shadow map generation)
//
// Vertex layout: pos(3) + uv(2) + normal(3) + tangent(3)
//
// Textures:
// - Albedo: RGB color (2D or cubemap)
// - Normal: Tangent-space normal map
// - Roughness: Grayscale roughness value

@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};

// Sphere mode imports
@if(sphere_mode)
import package::common::eac::{sampleEACCubemap, directionToEACFaceUV};

// Per-object uniforms (simpler than displaced_mesh - no displacement params)
struct NormalmappedUniforms {
    modelMatrix: mat4x4<f32>,
}

// Texture bindings differ based on mode
@if(!sphere_mode)
@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@if(sphere_mode)
@group(0) @binding(1) var albedo_cubemap: texture_cube<f32>;
@group(0) @binding(2) var material_sampler: sampler;

@if(!sphere_mode)
@group(0) @binding(3) var normal_texture: texture_2d<f32>;
@if(sphere_mode)
@group(0) @binding(3) var normal_cubemap: texture_cube<f32>;

@if(!sphere_mode)
@group(0) @binding(4) var roughness_texture: texture_2d<f32>;
@if(sphere_mode)
@group(0) @binding(4) var roughness_cubemap: texture_cube<f32>;

@group(1) @binding(0) var<uniform> object: NormalmappedUniforms;

// Vertex input differs based on mode:
// - flat_2d mode: pos(3) + uv(2) + normal(3) + tangent(3) at locations 0,1,2,3
// - sphere_mode:  pos(3) + normal(3) + tangent(3) at locations 0,1,2 (no UV needed)
@if(!sphere_mode)
struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}
@if(sphere_mode)
struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) tangent: vec3<f32>,
}

// GBuffer pass vertex output - 2D mode
@if(!shadow_pass && !sphere_mode)
struct VertexOutputGBuffer {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) worldTangent: vec3<f32>,
}

// GBuffer pass vertex output - sphere mode
@if(!shadow_pass && sphere_mode)
struct VertexOutputGBuffer {
    @builtin(position) position: vec4<f32>,
    @location(0) direction: vec3<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) worldTangent: vec3<f32>,
}

// Shadow pass vertex output (position only)
@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

// 2D mode: GBuffer pass vertex shader
@if(!shadow_pass && !sphere_mode)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;

    // No displacement - just transform position
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);

    // Transform normal and tangent to world space
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldTangent = normalize((object.modelMatrix * vec4<f32>(input.tangent, 0.0)).xyz);
    output.uv = input.uv;

    return output;
}

// Sphere mode: GBuffer pass vertex shader
@if(!shadow_pass && sphere_mode)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputGBuffer {
    var output: VertexOutputGBuffer;

    let dir = normalize(input.pos);

    // No displacement - just transform position
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);

    // For sphere, normal is same as direction (in local space)
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(dir, 0.0)).xyz);
    output.worldTangent = normalize((object.modelMatrix * vec4<f32>(input.tangent, 0.0)).xyz);
    output.direction = dir;

    return output;
}

// 2D mode: Shadow pass vertex shader
@if(shadow_pass && !sphere_mode)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;

    // No displacement - just transform position
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world_offset_pos);

    return output;
}

// Sphere mode: Shadow pass vertex shader
@if(shadow_pass && sphere_mode)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;

    // No displacement - just transform position
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world_offset_pos);

    return output;
}

// 2D mode: GBuffer pass fragment shader
@if(!shadow_pass && !sphere_mode)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    var output: GBufferOutput;

    // Reconstruct TBN matrix from interpolated vectors
    let N = normalize(input.worldNormal);
    let T = normalize(input.worldTangent - N * dot(input.worldTangent, N)); // Gram-Schmidt
    let B = cross(N, T);
    let TBN = mat3x3<f32>(T, B, N);

    // Sample textures
    let albedo = textureSample(albedo_texture, material_sampler, input.uv).rgb;
    let normalMap = textureSample(normal_texture, material_sampler, input.uv).rgb;
    let roughness = textureSample(roughness_texture, material_sampler, input.uv).r;

    // Unpack normal from [0,1] to [-1,1] range and transform to world space
    let tangentNormal = normalMap * 2.0 - 1.0;
    let worldNormal = normalize(TBN * tangentNormal);

    output.normals = encodeOctahedron(worldNormal);
    output.albedo = vec4<f32>(albedo, 1.0);
    // Material: R=roughness, G=shadow (1.0 = no shadow), B/A=reserved
    output.material = vec4<f32>(roughness, 1.0, 0.0, 0.0);

    return output;
}

// Sphere mode: GBuffer pass fragment shader
@if(!shadow_pass && sphere_mode)
@fragment
fn fs_main(input: VertexOutputGBuffer) -> GBufferOutput {
    var output: GBufferOutput;

    let dir = normalize(input.direction);

    // Reconstruct TBN matrix from interpolated vectors
    let N = normalize(input.worldNormal);
    let T = normalize(input.worldTangent - N * dot(input.worldTangent, N));
    let B = cross(N, T);
    let TBN = mat3x3<f32>(T, B, N);

    // Sample cubemap textures
    let albedo = sampleEACCubemap(albedo_cubemap, material_sampler, dir).rgb;
    let normalMap = sampleEACCubemap(normal_cubemap, material_sampler, dir).rgb;
    let roughness = sampleEACCubemap(roughness_cubemap, material_sampler, dir).r;

    // Unpack normal from [0,1] to [-1,1] range and transform to world space
    let tangentNormal = normalMap * 2.0 - 1.0;
    let worldNormal = normalize(TBN * tangentNormal);

    output.normals = encodeOctahedron(worldNormal);
    output.albedo = vec4<f32>(albedo, 1.0);
    // Material: R=roughness, G=shadow (1.0 = no shadow), B/A=reserved
    output.material = vec4<f32>(roughness, 1.0, 0.0, 0.0);

    return output;
}

// Shadow pass fragment shader (depth-only)
@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    // Depth is written automatically via @builtin(position)
}
