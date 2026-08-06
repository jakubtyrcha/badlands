// Water surface material.
//
// The water BODY is a homogeneous single-scattering medium, not a tint. Along
// the view ray inside the water, with path length d metres:
//
//     T        = exp(-sigma_e * d)                    per-channel transmittance
//     L_in     = scatterAlbedo * (skySH + shadowedSun)   the in-scattered murk
//     L_under  = bg * T + L_in * (1 - T)
//
// Two things fall straight out of that rather than being dialled in: at d -> 0
// the result IS the lake bed (so the shoreline needs no coast-width fade, and
// the surface can output alpha 1), and because sigma_e is per-channel the hue
// slides toward the murk asymptote as the water deepens. sigma_e is stated as a
// visibility depth in metres and converted (sigma = 3 / d_vis) -- see
// water_material.hpp's presets.
//
// Variants (selected by MaterialPassType -> shader features, see
// standard_material_factory.cpp kVariants, plus FactoryDescriptor::extra_features):
//   - "transparent" (kForwardTransparent): the production surface — vertex wave
//     displacement, per-pixel normal = analytic wave normal + procedural
//     high-freq detail (TBN), sun GGX specular + IBL reflection, normal-driven
//     distortion of the copied scene colour (refraction), and the medium above.
//     Premultiplied output. Does NOT write depth (FactoryDescriptor
//     depth_write=false); rendered against a depth-read-only attachment.
//   - "still" (extra feature, forward-transparent only): standing water. No wave
//     displacement, shading normal pinned to +Y. Everything else — the medium,
//     the sky reflection, the sun glint, shadowing — is unchanged. Refraction
//     vanishes for free, since its offset is driven by N.xz.
//   - default (kGBuffer): writes encodeOctahedron(analytic wave normal) + albedo
//     + material to the G-buffer. Used by badlands_water_gpu_test to read the
//     GPU wave normal back and cross-check src/engine/rendering/water_waves.hpp,
//     and usable as a normals debug view. No detail perturbation here — this is
//     exactly the analytic wave normal the CPU core predicts.
//   - "shadow_pass" (kShadow): depth-only (not a shadow caster in practice, but
//     present so the kDeferred variant's shadow pipeline compiles). `still` never
//     reaches it: kForwardTransparent registers only RenderPassType::kForward.
//
// Vertex layout: pos(3)+uv(2)+normal(3)+tangent(4); waves are a function of TRUE
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
@if(transparent && !still)
import package::common::water_noise::{waterDetailNormalTS};
import package::common::water_waves::{waterWaveHeight, waterWaveNormal};

// Optical path length used when nothing is behind the water (the sky, at the
// far edge of the map). Deep enough that transmittance is negligible for any
// plausible extinction, so such a fragment reads as open water rather than
// glass.
const kSkyBehindDepthM: f32 = 32.0;

// How much water the view ray must cross before refraction reaches full
// strength. A millimetre-deep film at the waterline must not smear the bed
// sideways, so the distortion ramps in with PATH LENGTH -- a length scale in
// metres, deliberately not a transmittance, which would tie the geometry of the
// refraction to whichever colour channel happened to be sampled.
const kRefractRampM: f32 = 1.5;

struct WaterUniforms {
    modelMatrix: mat4x4<f32>,
    time: f32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    // Per-channel extinction coefficient, 1/m. Derived from a visibility depth:
    // sigma = 3 / d_vis, so the bed has faded by ~95% at d_vis metres. Red is
    // the largest, which is what turns the water toward blue as it deepens.
    extinction: vec4<f32>,
    // Single-scattering albedo (0..1): the fraction of extinguished light that
    // is re-scattered toward the viewer rather than absorbed. Deep water
    // asymptotes to this, lit by the sky + shadowed sun.
    scatterAlbedo: vec4<f32>,
    params: vec4<f32>,        // x=refractStrength, y=roughness, z/w reserved
    params2: vec4<f32>,       // x=detailStrength, y=blockout flag
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

@if(!shadow_pass && !still)
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

// Standing water: the surface holds its rest plane, so this is the plain
// transform. worldXZ is still carried so the fragment stage sees one
// VertexOutput shape across both variants.
@if(!shadow_pass && still)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let base = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(base);
    output.worldOffsetPos = base;
    output.worldXZ = base.xz + frame.cameraWorldPos.xz;
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

// World-space shading normal. Split at FUNCTION granularity rather than with an
// @if inside the body: every @if in shaders/ is item-level, and there is no
// statement-level precedent to lean on.
@if(transparent && !still)
fn waterShadingNormal(worldXZ: vec2<f32>) -> vec3<f32> {
    // Blockout/flat mode (params2.y > 0.5): a dead-flat +Y shading normal for
    // the greybox look (the vertex stage still displaces by the waves, so the
    // surface undulates but shades flat). Uniform branch (params2.y is a
    // uniform), so the caller's texture sampling stays in uniform control flow.
    if (object.params2.y > 0.5) {
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    // Base wave normal (per-fragment), enriched with a tangent-space detail normal.
    let Nwave = waterWaveNormal(worldXZ, object.time);
    let ts = waterDetailNormalTS(worldXZ, object.time, object.params2.x);
    let T = normalize(cross(vec3<f32>(0.0, 0.0, 1.0), Nwave));
    let B = cross(Nwave, T);
    return normalize(T * ts.x + B * ts.y + Nwave * ts.z);
}

// Standing water: dead flat. Refraction's screen offset is driven by N.xz, so
// it vanishes here for free -- no bed smearing across the waterline.
@if(transparent && still)
fn waterShadingNormal(worldXZ: vec2<f32>) -> vec3<f32> {
    return vec3<f32>(0.0, 1.0, 0.0);
}

// === G-buffer variant: write the analytic wave normal (cross-checked) ===
@if(!shadow_pass && !transparent)
@fragment
fn fs_main(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    // Recompute the wave normal per-fragment from interpolated world XZ (exact
    // for a planar XZ surface) — this is what water_waves.hpp predicts.
    let N = waterWaveNormal(input.worldXZ, object.time);
    output.normals = encodeOctahedron(N);
    output.albedo = vec4<f32>(object.scatterAlbedo.rgb, 1.0);
    output.material = vec4<f32>(object.params.y, 1.0, 1.0, 0.0);  // roughness, shadow, ao
    return output;
}

// === Forward transparent variant: medium + reflection + distortion ===
@if(transparent)
@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let N = waterShadingNormal(input.worldXZ);

    let V = normalize(-input.worldOffsetPos);  // camera at origin in offset space
    let NdotV = max(dot(N, V), 1e-3);

    // Optical path length through the medium, along the VIEW RAY (reads scene
    // depth; does not modify it). This is the physically right length for
    // Beer-Lambert: at a grazing angle the ray travels further through the
    // water than the water is deep, and it correctly goes to zero at the
    // waterline.
    let pixel = vec2<i32>(input.position.xy);
    let sceneDepthRaw = textureLoad(sceneDepth, pixel, 0);
    let waterZ = reconstructLinearZ(input.position.z);
    var pathM = kSkyBehindDepthM;
    if (isValidDepth(sceneDepthRaw)) {
        pathM = max(reconstructLinearZ(sceneDepthRaw) - waterZ, 0.0);
    }
    let T = exp(-object.extinction.xyz * pathM);
    // Ramps the refraction in with water thickness so a shallow shoreline does
    // not smear the bed across the waterline. Driven by path length, NOT by
    // transmittance: a medium can be optically clear and still be metres deep,
    // and the distortion is a property of the geometry, not of the colour.
    let refractRamp = clamp(pathM / kRefractRampM, 0.0, 1.0);

    // Refraction: normal-driven distortion of the copied scene colour. Reject
    // the offset when it lands on foreground geometry (a surface CLOSER than the
    // water), so objects in front of the lake don't bleed across the surface.
    let screenUV = input.position.xy / frame.screenSize;
    let refrUV = clamp(screenUV + N.xz * object.params.x * refractRamp,
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

    // Light available INSIDE the medium: the sky (SH, unshadowed) plus the
    // SHADOWED sun, so the murk tracks time of day and darkens in shadow. `bg`
    // (the refracted background) is already lit; only the in-scattering needs
    // this.
    let envLight = evaluateAmbientSHL2(N, frame.ambientSH)
                   + NdotL * frame.sunColor.rgb * shadow;
    let L_in = object.scatterAlbedo.rgb * envLight;

    // Single scatter through a homogeneous medium: the bed attenuates, the murk
    // accumulates. At pathM = 0 this is EXACTLY bg, which is why the shoreline
    // needs no separate coast fade.
    let transmitted = bg * T + L_in * (vec3<f32>(1.0) - T);

    // Reflection: a SINGLE Fresnel (split-sum LUT: ~F0 head-on, ~1 grazing) mixes
    // the transmitted colour with the raw sky IBL, so the reflection shows the sky
    // and grows at grazing angles (and tracks time of day via the prefiltered env).
    let R = reflect(-V, N);
    let maxMip = f32(textureNumLevels(iblPrefiltered) - 1u);
    let roughness = object.params.y;
    let reflColor = textureSampleLevel(iblPrefiltered, iblSampler, R, roughness * maxMip).rgb;
    let F0 = vec3<f32>(0.02);
    let brdf = textureSampleLevel(brdfLut, brdfLutSampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    // Clamp so the split-sum term never exceeds 1 (F0*scale can push it slightly
    // over at grazing) — an unclamped mix factor would extrapolate past reflColor.
    var fresnel = clamp(F0 * brdf.x + vec3<f32>(brdf.y), vec3<f32>(0.0), vec3<f32>(1.0));
    var sunSpec = evaluateSpecularGgxBrdf(N, V, L, roughness, F0) * NdotL * frame.sunColor.rgb * shadow;
    // Blockout/flat mode: no sky reflection, no sun glint — just the medium.
    // reflColor/brdf are still sampled above so this pipeline's @group(2) layout
    // is identical to the detailed variant (the forward-transparent pass binds
    // all 9 engine resources unconditionally — see RenderForwardTransparentMeshes).
    if (object.params2.y > 0.5) {
        fresnel = vec3<f32>(0.0);
        sunSpec = vec3<f32>(0.0);
    }

    let rgb = mix(transmitted, reflColor, fresnel) + sunSpec;

    // Opaque. The transmittance IS the shore fade: `transmitted` already reduces
    // to the copied background at zero path length, so blending it again against
    // that same background would only double-count it.
    return vec4<f32>(rgb, 1.0);  // premultiplied, alpha = 1
}

// === Shadow variant: depth-only ===
@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
}
