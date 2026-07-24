// General forward-opaque, standard-lit material. Receives the sun with the SAME
// standard BRDF as the deferred pass (shared shadeStandard) + shadow-map PCF +
// IBL, so it "follows the G-buffer". Alpha-tested (params.x = cutoff; 0 = never
// discards -> plain opaque); shaded double-sided when the material is cull-None
// (foliage). Casts an (alpha-tested) shadow via the shadow_pass variant.
// Vertex layout: pos(3)+uv(2)+normal(3)+tangent(3).

@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::color::srgb_to_linear;
@if(!shadow_pass)
import package::common::shadow_sampling::sampleShadowMapPCF;
@if(!shadow_pass)
import package::common::standard_lighting::{shadeStandard, computeSpecularOcclusion, F0_DIELECTRIC, evaluateTranslucency};

@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var material_sampler: sampler;

struct StandardForwardUniforms {
    modelMatrix: mat4x4<f32>,
    tint: vec4<f32>,     // rgb = albedo tint
    params: vec4<f32>,   // x = alpha cutoff (0 = opaque), y = roughness
    transmission: vec4<f32>,   // rgb = transmission tint, a = translucency strength
}
@group(1) @binding(0) var<uniform> object: StandardForwardUniforms;

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
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world);
    output.worldOffsetPos = world;
    output.uv = input.uv;
    output.worldNormal = (object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz;
    return output;
}
@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world);
    output.uv = input.uv;
    return output;
}

@if(!shadow_pass && translucency)
fn transmissionContribution(N: vec3<f32>, V: vec3<f32>, ao: f32, shadow: f32) -> vec3<f32> {
    let L = normalize(frame.sunDirection.xyz);
    return evaluateTranslucency(N, V, L, shadow, ao,
                                object.transmission.rgb, object.transmission.a);
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

    if (tex.a < object.params.x) { discard; }

    let albedoLinear = srgb_to_linear(tex.rgb) * object.tint.rgb;
    let roughness = object.params.y;
    let ao = 1.0;

    // IBL split-sum ambient specular (explicit-LOD -> no derivatives).
    let R = reflect(-V, N);
    let NdotV = max(dot(N, V), 0.0);
    let maxMip = f32(textureNumLevels(prefiltered_env) - 1u);
    let prefilteredColor = textureSampleLevel(prefiltered_env, prefiltered_sampler, R, roughness * maxMip).rgb;
    let brdfLutSample = textureSampleLevel(brdf_lut, brdf_lut_sampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    let specularScale = F0_DIELECTRIC * brdfLutSample.r + brdfLutSample.g;
    let ambientSpecular = prefilteredColor * specularScale * computeSpecularOcclusion(NdotV, ao, roughness);

    let color = shadeStandard(albedoLinear, N, V, roughness, ao, shadow, ambientSpecular);
    let transmission = transmissionContribution(N, V, ao, shadow);
    return vec4<f32>(color + transmission, 1.0);
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    let a = textureSampleLevel(albedo_texture, material_sampler, input.uv, 0.0).a;
    if (a < object.params.x) { discard; }
}
