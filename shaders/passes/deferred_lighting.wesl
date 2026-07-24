// Deferred lighting pass — sun directional (EON diffuse + GGX specular) +
// spherical-harmonic ambient diffuse + IBL ambient specular (split-sum),
// evaluated in World-Camera-Offseted Space.
//
// Adapted from sampo's src/shaders/passes/deferred_lighting.wesl. Kept:
// world-pos reconstruction from reversed-Z depth, octahedron normal decode, the
// sun directional term (EON retro diffuse + GGX spec), the SH ambient diffuse
// term, (task S2.B2) the IBL ambient-specular split-sum term, (task M6)
// screen-space GTAO (binding 7): final AO = min(gtao, baked-AO), and (task T3)
// the directional shadow-map PCF term (binding 5/6) combined with the (still
// T5's no-op) contact-shadow texture (binding 8) via min().
//
// T3 group-0 layout: frame UBO @0, gbuffer depth @1, normals @2, albedo @3,
// material @4 (all textureLoad'd), shadow map @5 + comparison sampler @6,
// GTAO ao @7, contact shadow @8 (textureLoad'd), prefiltered env cube @9 +
// its sampler @10, BRDF split-sum LUT @11 + its sampler @12. The
// SceneRenderer ALWAYS binds @5/@6 (shadow_map_'s depth view + the shadow
// comparison sampler), @7 (a GTAO texture, or a 1.0-cleared texture when GTAO
// is off), @8 (contact_shadow_view_, 1.0-cleared no-op until T5), and @9..@12
// (prefiltered cube or a 1x1 black fallback cube + the BRDF LUT), so this
// layout is always satisfiable.
import package::common::frame::{frame, getFullscreenTriangleVertex, reconstructLinearZ, reconstructViewSpacePosition, viewSpaceToWorldOffsetedSpacePosition, decodeOctahedron};
import package::common::color::srgb_to_linear;
import package::common::shadow_sampling::sampleShadowMapPCF;
import package::common::standard_lighting::{shadeStandard, computeSpecularOcclusion, F0_DIELECTRIC};

@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var normals_texture: texture_2d<f32>;
@group(0) @binding(3) var albedo_texture: texture_2d<f32>;
@group(0) @binding(4) var material_texture: texture_2d<f32>;  // RGBA8: R=roughness, G=shadow, B=baked AO
@group(0) @binding(5) var shadow_map: texture_depth_2d;         // Directional shadow map (conventional Z)
@group(0) @binding(6) var shadow_sampler: sampler_comparison;   // LessEqual compare (T3)
@group(0) @binding(7) var ao_texture: texture_2d<f32>;          // GTAO screen-space AO (R8: 1=unoccluded)
@group(0) @binding(8) var contact_shadow_texture: texture_2d<f32>; // Screen-space contact shadow (R8: 1=no occlusion; T5 no-op until then)
@group(0) @binding(9) var prefiltered_env: texture_cube<f32>;   // IBL prefiltered environment
@group(0) @binding(10) var prefiltered_sampler: sampler;
@group(0) @binding(11) var brdf_lut: texture_2d<f32>;           // Split-sum BRDF LUT (R=scale, G=bias)
@group(0) @binding(12) var brdf_lut_sampler: sampler;

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
    @location(0) screenUV: vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    let fullscreen = getFullscreenTriangleVertex(VertexIndex);
    var output: VertexOutput;
    output.Position = fullscreen.clipPosition;
    output.screenUV = fullscreen.screenUV;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);
    let depth = textureLoad(depth_texture, texCoord, 0);
    let linearZ = reconstructLinearZ(depth);

    // 1. Reconstruct Position in World-Camera-Offseted Space (camera at origin)
    let posInViewSpace = reconstructViewSpacePosition(input.screenUV, depth);
    let posOffseted = viewSpaceToWorldOffsetedSpacePosition(posInViewSpace);

    // 2. Prepare vectors (all in Offseted Space)
    let normalOct = textureLoad(normals_texture, texCoord, 0).xy;
    let normal = decodeOctahedron(normalOct); // directions are space-invariant

    let lightDir = normalize(frame.sunDirection.xyz);
    // In Offseted Space, the camera is at (0,0,0)
    let viewDir = normalize(-posOffseted);
    let NdotV = max(dot(normal, viewDir), 0.0);
    let NdotL = max(dot(normal, lightDir), 0.0);

    // 3. Shadowing (T3): sun shadow = min(shadow-map RPDB PCF, contact
    // shadow). The contact term stays a 1.0 no-op until T5's SSCS pass
    // writes it, so shadow == shadowMapPCF for now. worldPos (not Offseted
    // Space) is what frame.lightViewProj expects — see
    // worldCameraOffsetedSpaceToWorldSpace in frame.wesl.
    //
    // Computed HERE, before the sky/no-geometry discard below: WGSL requires
    // dpdx/dpdy/textureSampleCompare (used inside sampleShadowMapPCF) to run
    // in uniform control flow, and a discard gated on a per-pixel value
    // (linearZ, from a texture load) is exactly the kind of non-uniform
    // branch that would taint everything textually after it — so the whole
    // shadow term must be evaluated before that branch exists, even though
    // its result is unused/discarded for sky pixels.
    let worldPos = posOffseted + frame.cameraWorldPos.xyz;
    let shadowMapPCF = sampleShadowMapPCF(worldPos, normal, NdotL, shadow_map, shadow_sampler);
    let contactShadow = textureLoad(contact_shadow_texture, texCoord, 0).r;
    let shadow = min(shadowMapPCF, contactShadow);

    // Sky / no-geometry: leave the pass's clear color untouched.
    if (linearZ >= frame.farPlane * 0.999) {
        discard;
    }

    // 4. Material properties from GBuffer (R=roughness) + occlusion. Final
    // AO = min(screen-space GTAO, baked AO). `enableGtao` gates whether the
    // GTAO compute pass ran this frame: when it didn't, gtao=1.0 so
    // ao = min(1.0, baked) = baked (M4 behaviour).
    let materialData = textureLoad(material_texture, texCoord, 0);
    let roughness = materialData.r;
    let gtao = select(1.0, textureLoad(ao_texture, texCoord, 0).r, frame.enableGtao != 0u);
    let ao = min(gtao, materialData.b);

    // 5. Lighting calculation
    let albedoLinear = srgb_to_linear(textureLoad(albedo_texture, texCoord, 0).rgb);

    // Ambient specular (IBL). Sample the prefiltered environment cube along the
    // reflection vector at a roughness-selected mip, then apply the split-sum
    // BRDF LUT (indexed by NdotV, roughness) — specular = F0 * lut.r + lut.g.
    let R = reflect(-viewDir, normal);
    let max_mip = f32(textureNumLevels(prefiltered_env) - 1u);
    let mip_level = roughness * max_mip;
    let prefilteredColor = textureSampleLevel(prefiltered_env, prefiltered_sampler, R, mip_level).rgb;

    let brdfLutSample = textureSampleLevel(brdf_lut, brdf_lut_sampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    let specularScale = F0_DIELECTRIC * brdfLutSample.r + brdfLutSample.g;
    // Specular occlusion: roughness/view-angle-aware AO for the IBL specular
    // term (prevents ambient specular leaking into cracks — sampo parity).
    let specOcclusion = computeSpecularOcclusion(NdotV, ao, roughness);
    let ambientSpecular = prefilteredColor * specularScale * specOcclusion;

    let finalColorLinear = shadeStandard(albedoLinear, normal, viewDir, roughness, ao, shadow, ambientSpecular);

    // Directional-shadow debug visualization (T1 plumbing, wired up by T3;
    // see ShadowDebugMode in scene_renderer.hpp). Uniform branch
    // (frame.debugFlags is the same for every pixel this frame), so this does
    // not diverge per-fragment; shadow/shadowMapPCF/contactShadow are already
    // computed unconditionally above (the derivatives/compare inside
    // sampleShadowMapPCF never sit behind this branch).
    if (frame.debugFlags != 0u) {
        var debugValue = 1.0;
        if (frame.debugFlags == 1u) {
            debugValue = shadow;              // Combined: min(shadowMapPCF, contactShadow)
        } else if (frame.debugFlags == 2u) {
            debugValue = shadowMapPCF;        // ShadowMapOnly
        } else if (frame.debugFlags == 3u) {
            debugValue = contactShadow;       // ContactOnly
        }
        return vec4<f32>(debugValue, debugValue, debugValue, 1.0);
    }

    return vec4<f32>(finalColorLinear, 1.0);
}
