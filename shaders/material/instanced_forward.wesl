// Instanced forward-opaque, standard-lit material. Forked from
// standard_forward.wesl for the GPU-driven instanced-rendering feature: the
// fragment stage (shadow-map PCF + IBL + shadeStandard assembly via the shared
// shadeStandardForward helper + the @if(translucency) transmission path)
// matches standard_forward.wesl's; only the vertex stage + group layout
// change. The per-object model matrix is read per-instance from a group-1
// storage array indexed by @builtin(instance_index); material constants (tint,
// params, transmission) live in a group-0 UBO.
//
// Feature: `instanced`. Vertex layout: pos(3)+uv(2)+normal(3)+tangent(3).

@if(!shadow_pass)
import package::common::frame::{frame, worldSpaceToClipSpace, worldSpaceToWorldCameraOffsetedSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldSpaceToWorldCameraOffsetedSpace, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::color::srgb_to_linear;
@if(!shadow_pass)
import package::common::shadow_sampling::sampleShadowMapPCF;
@if(!shadow_pass)
import package::common::standard_lighting::{shadeStandardForward, evaluateTranslucency};

// Group 0: frame UBO (@0, imported) + albedo texture + sampler + material
// constants UBO (@3). The model matrix is NOT here — it is per-instance.
@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var material_sampler: sampler;

struct MaterialParams {
    tint: vec4<f32>,           // rgb = albedo tint
    params: vec4<f32>,         // x = alpha cutoff (0 = opaque), y = roughness
    transmission: vec4<f32>,   // rgb = transmission tint, a = translucency strength
    bucketId: u32,             // which (model,lod) bucket THIS draw renders (Phase D)
}
@group(0) @binding(3) var<uniform> mat_params: MaterialParams;

// Group 1: the WHOLE compacted per-(model,lod) transform buffer (@0) plus the
// GPU-computed per-bucket base offsets (@1). The vertex stage recovers this
// draw's transforms as `instances[bucketBase[mat_params.bucketId] +
// instance_index]` — the CPU knows bucketId per draw but NOT the prefix-summed
// base, so the base is read on the GPU (no per-frame offset readback). Phase C's
// single-bucket path is the trivial case: bucketBase = [0], bucketId = 0.
@group(1) @binding(0) var<storage, read> instances: array<mat4x4<f32>>;
@group(1) @binding(1) var<storage, read> bucketBase: array<u32>;

// Group 2: engine resources (shadow map + IBL), identical 6-entry set to
// standard_forward.wesl's forward-opaque group.
@if(!shadow_pass) @group(2) @binding(0) var shadow_map: texture_depth_2d;
@if(!shadow_pass) @group(2) @binding(1) var shadow_sampler: sampler_comparison;
@if(!shadow_pass) @group(2) @binding(2) var prefiltered_env: texture_cube<f32>;
@if(!shadow_pass) @group(2) @binding(3) var prefiltered_sampler: sampler;
@if(!shadow_pass) @group(2) @binding(4) var brdf_lut: texture_2d<f32>;
@if(!shadow_pass) @group(2) @binding(5) var brdf_lut_sampler: sampler;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}
@if(!shadow_pass)
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldOffsetPos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) worldNormal: vec3<f32>,
}
@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    var output: VertexOutput;
    let base = bucketBase[mat_params.bucketId];
    let m = instances[base + instance_index];
    // Instance matrices hold FULL world transforms.
    let world = (m * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldSpaceToClipSpace(world);
    // worldOffsetPos is camera-offseted (the FS re-adds cameraWorldPos to get
    // the world position for shadow sampling, and derives V from -offsetPos).
    output.worldOffsetPos = worldSpaceToWorldCameraOffsetedSpace(world);
    output.uv = input.uv;
    output.worldNormal = (m * vec4<f32>(input.normal, 0.0)).xyz;
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
    output.uv = input.uv;
    return output;
}

@if(!shadow_pass && translucency)
fn transmissionContribution(N: vec3<f32>, V: vec3<f32>, ao: f32, shadow: f32) -> vec3<f32> {
    let L = normalize(frame.sunDirection.xyz);
    return evaluateTranslucency(N, V, L, shadow, ao,
                                mat_params.transmission.rgb, mat_params.transmission.a);
}
@if(!shadow_pass && !translucency)
fn transmissionContribution(N: vec3<f32>, V: vec3<f32>, ao: f32, shadow: f32) -> vec3<f32> {
    return vec3<f32>(0.0);
}

@if(!shadow_pass)
@fragment
fn fs_main(input: VertexOutput, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    // Albedo + shadow use implicit derivatives -> sample BEFORE the (non-uniform)
    // alpha-test discard to keep derivative uniformity.
    let tex = textureSample(albedo_texture, material_sampler, input.uv);

    var N = normalize(input.worldNormal);
    if (!front) { N = -N; }  // double-sided (harmless single-sided: back faces culled)

    let V = normalize(-input.worldOffsetPos);
    let L = normalize(frame.sunDirection.xyz);
    let NdotL = max(dot(N, L), 0.0);
    let worldPos = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let shadow = sampleShadowMapPCF(worldPos, N, NdotL, shadow_map, shadow_sampler);

    if (tex.a < mat_params.params.x) { discard; }

    let albedoLinear = srgb_to_linear(tex.rgb) * mat_params.tint.rgb;
    let roughness = mat_params.params.y;
    let ao = 1.0;

    let color = shadeStandardForward(albedoLinear, N, V, roughness, ao, shadow,
        prefiltered_env, prefiltered_sampler, brdf_lut, brdf_lut_sampler);
    let transmission = transmissionContribution(N, V, ao, shadow);
    return vec4<f32>(color + transmission, 1.0);
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    let a = textureSampleLevel(albedo_texture, material_sampler, input.uv, 0.0).a;
    if (a < mat_params.params.x) { discard; }
}
