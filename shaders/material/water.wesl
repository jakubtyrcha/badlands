// Water surface material.
//
// Variants (selected by MaterialPassType -> shader features, see
// standard_material_factory.cpp kVariants):
//   - "transparent" (kForwardTransparent): the production surface — vertex wave
//     displacement, per-pixel normal = analytic wave normal + procedural
//     high-freq detail (TBN), sun GGX specular + IBL reflection, normal-driven
//     distortion of the copied scene colour (refraction), and depth-based coast
//     / absorption. Premultiplied output. Does NOT write depth (FactoryDescriptor
//     depth_write=false); rendered against a depth-read-only attachment.
//   - default (kGBuffer): writes encodeOctahedron(analytic wave normal) + albedo
//     + material to the G-buffer. Used by badlands_water_gpu_test to read the
//     GPU wave normal back and cross-check src/engine/rendering/water_waves.hpp,
//     and usable as a normals debug view. No detail perturbation here — this is
//     exactly the analytic wave normal the CPU core predicts.
//   - "shadow_pass" (kShadow): depth-only (not a shadow caster in practice, but
//     present so the kDeferred variant's shadow pipeline compiles).
//
// Vertex layout: pos(3)+uv(2)+normal(3)+tangent(3); waves are a function of TRUE
// world XZ (offset + camera) so they are camera-stationary.

@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace, reconstructLinearZ, isValidDepth};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass && !transparent)
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};
@if(transparent)
import package::common::gbuffer_encode::{encodeOctahedron};
@if(transparent)
import package::common::brdf::{evaluateSpecularGgxBrdf};
@if(transparent)
import package::common::sh_lighting::{evaluateAmbientSHL2};
@if(transparent)
import package::common::shadow_sampling::{sampleShadowMapPCF};
@if(transparent)
import package::common::water_noise::{waterDetailNormalTS};
import package::common::water_waves::{waterWaveHeight, waterWaveNormal};

struct WaterUniforms {
    modelMatrix: mat4x4<f32>,
    time: f32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    deepColor: vec4<f32>,     // xyz = deep-water tint
    shallowColor: vec4<f32>,  // xyz = shallow-water tint
    params: vec4<f32>,        // x=absorption, y=refractStrength, z=coastWidth, w=roughness
    params2: vec4<f32>,       // x=detailStrength
}

@group(1) @binding(0) var<uniform> object: WaterUniforms;

// Engine resources (transparent/forward variant only), bound at group 2 by
// RenderForwardTransparentMeshes.
@if(transparent) @group(2) @binding(0) var sceneDepth: texture_depth_2d;
@if(transparent) @group(2) @binding(1) var sceneColor: texture_2d<f32>;
@if(transparent) @group(2) @binding(2) var sceneColorSampler: sampler;
@if(transparent) @group(2) @binding(3) var iblPrefiltered: texture_cube<f32>;
@if(transparent) @group(2) @binding(4) var iblSampler: sampler;
@if(transparent) @group(2) @binding(5) var brdfLut: texture_2d<f32>;
@if(transparent) @group(2) @binding(6) var brdfLutSampler: sampler;
@if(transparent) @group(2) @binding(7) var shadowMap: texture_depth_2d;
@if(transparent) @group(2) @binding(8) var shadowSampler: sampler_comparison;

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
    @location(1) worldXZ: vec2<f32>,
}

@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

// Displace the transformed position's Y by the wave height at true world XZ.
fn waterWorldOffset(localPos: vec3<f32>) -> vec3<f32> {
    let base = (object.modelMatrix * vec4<f32>(localPos, 1.0)).xyz;
    let worldXZ = base.xz + frame.cameraWorldPos.xz;
    let h = waterWaveHeight(worldXZ, object.time);
    return vec3<f32>(base.x, base.y + h, base.z);
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let base = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    let worldXZ = base.xz + frame.cameraWorldPos.xz;
    let h = waterWaveHeight(worldXZ, object.time);
    let worldOffset = vec3<f32>(base.x, base.y + h, base.z);
    output.position = worldCameraOffsetedSpaceToClipSpace(worldOffset);
    output.worldOffsetPos = worldOffset;
    output.worldXZ = worldXZ;
    return output;
}

@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(waterWorldOffset(input.pos));
    return output;
}

// The high-frequency detail normal (fluid fBm + domain warp) lives in
// shaders/common/water_noise.wesl (waterDetailNormalTS), forward-only.

// === G-buffer variant: write the analytic wave normal (cross-checked) ===
@if(!shadow_pass && !transparent)
@fragment
fn fs_main(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    // Recompute the wave normal per-fragment from interpolated world XZ (exact
    // for a planar XZ surface) — this is what water_waves.hpp predicts.
    let N = waterWaveNormal(input.worldXZ, object.time);
    output.normals = encodeOctahedron(N);
    output.albedo = vec4<f32>(object.shallowColor.rgb, 1.0);
    output.material = vec4<f32>(object.params.w, 1.0, 1.0, 0.0);  // roughness, shadow, ao
    return output;
}

// === Forward transparent variant: lit + reflection + distortion + coast ===
@if(transparent)
@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    // Blockout/flat mode (params2.y > 0.5): a dead-flat +Y shading normal — no
    // wave or detail perturbation — for the greybox look (the vertex stage still
    // displaces by the waves, so the surface undulates but shades flat). Uniform
    // branch (params2.y is a uniform), so the texture sampling below stays in
    // uniform control flow.
    var N = vec3<f32>(0.0, 1.0, 0.0);
    if (object.params2.y <= 0.5) {
        // Base wave normal (per-fragment), enriched with a tangent-space detail normal.
        let Nwave = waterWaveNormal(input.worldXZ, object.time);
        let ts = waterDetailNormalTS(input.worldXZ, object.time, object.params2.x);
        let T = normalize(cross(vec3<f32>(0.0, 0.0, 1.0), Nwave));
        let B = cross(Nwave, T);
        N = normalize(T * ts.x + B * ts.y + Nwave * ts.z);
    }

    let V = normalize(-input.worldOffsetPos);  // camera at origin in offset space
    let NdotV = max(dot(N, V), 1e-3);

    // Depth-based coast / absorption (reads scene depth; does not modify it).
    let pixel = vec2<i32>(input.position.xy);
    let sceneDepthRaw = textureLoad(sceneDepth, pixel, 0);
    let waterZ = reconstructLinearZ(input.position.z);
    var waterDepth = object.params.z * 4.0;  // sky behind -> treat as deep
    if (isValidDepth(sceneDepthRaw)) {
        waterDepth = max(reconstructLinearZ(sceneDepthRaw) - waterZ, 0.0);
    }
    let coast = smoothstep(0.0, max(object.params.z, 1e-3), waterDepth);
    let absorb = 1.0 - exp(-waterDepth * object.params.x);

    // Refraction: normal-driven distortion of the copied scene colour. Reject
    // the offset when it lands on foreground geometry (a surface CLOSER than the
    // water), so objects in front of the lake don't bleed across the surface.
    let screenUV = input.position.xy / frame.screenSize;
    let refrUV = clamp(screenUV + N.xz * object.params.y * coast,
                       vec2<f32>(0.0), vec2<f32>(1.0));
    let offDepthRaw = textureLoad(sceneDepth, vec2<i32>(refrUV * frame.screenSize), 0);
    var bgUV = refrUV;
    if (isValidDepth(offDepthRaw) && reconstructLinearZ(offDepthRaw) < waterZ) {
        bgUV = screenUV;  // foreground at the offset -> sample straight through
    }
    let bg = textureSampleLevel(sceneColor, sceneColorSampler, bgUV, 0.0).rgb;
    let L = normalize(frame.sunDirection.xyz);
    let NdotL = max(dot(N, L), 0.0);

    // Sun shadow (PCF) at this water fragment's world position, so water in a
    // terrain/rim shadow loses the sun's contribution.
    let worldPos = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let shadow = sampleShadowMapPCF(worldPos, N, NdotL, shadowMap, shadowSampler);

    // Water body colour: absorption tint lit by the sky (SH, unshadowed) plus the
    // SHADOWED sun, so it tracks time of day and darkens inside shadow. `bg` (the
    // refracted background) is already lit; only the tint needs it.
    let envLight = evaluateAmbientSHL2(N, frame.ambientSH)
                   + NdotL * frame.sunColor.rgb * shadow;
    let waterTint = mix(object.shallowColor.rgb, object.deepColor.rgb, absorb) * envLight;

    // Scattering: sunlight forward-scattered through the water body toward the
    // viewer — shadowed, stronger looking into the sun and in deeper water.
    let scatter = object.shallowColor.rgb * frame.sunColor.rgb * shadow
                  * pow(max(dot(V, -L), 0.0), 4.0) * absorb;

    let transmitted = mix(bg, waterTint, absorb) + scatter;

    // Reflection: a SINGLE Fresnel (split-sum LUT: ~F0 head-on, ~1 grazing) mixes
    // the transmitted colour with the raw sky IBL, so the reflection shows the sky
    // and grows at grazing angles (and tracks time of day via the prefiltered env).
    let R = reflect(-V, N);
    let maxMip = f32(textureNumLevels(iblPrefiltered) - 1u);
    let roughness = object.params.w;
    let reflColor = textureSampleLevel(iblPrefiltered, iblSampler, R, roughness * maxMip).rgb;
    let F0 = vec3<f32>(0.02);
    let brdf = textureSampleLevel(brdfLut, brdfLutSampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    // Clamp so the split-sum term never exceeds 1 (F0*scale can push it slightly
    // over at grazing) — an unclamped mix factor would extrapolate past reflColor.
    var fresnel = clamp(F0 * brdf.x + vec3<f32>(brdf.y), vec3<f32>(0.0), vec3<f32>(1.0));
    var sunSpec = evaluateSpecularGgxBrdf(N, V, L, roughness, F0) * NdotL * frame.sunColor.rgb * shadow;
    // Blockout/flat mode: no sky reflection, no sun glint — just the depth tint.
    // reflColor/brdf are still sampled above so this pipeline's @group(2) layout
    // is identical to the detailed variant (the forward-transparent pass binds
    // all 9 engine resources unconditionally — see RenderForwardTransparentMeshes).
    if (object.params2.y > 0.5) {
        fresnel = vec3<f32>(0.0);
        sunSpec = vec3<f32>(0.0);
    }

    let rgb = mix(transmitted, reflColor, fresnel) + sunSpec;

    let alpha = clamp(coast, 0.0, 1.0);  // soft shoreline; interior saturates to 1
    return vec4<f32>(rgb * alpha, alpha);  // premultiplied
}

// === Shadow variant: depth-only ===
@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
}
