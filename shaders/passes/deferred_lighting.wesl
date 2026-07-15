// Deferred lighting pass — sun directional (EON diffuse + GGX specular) +
// spherical-harmonic ambient diffuse + IBL ambient specular (split-sum),
// evaluated in World-Camera-Offseted Space.
//
// Adapted from sampo's src/shaders/passes/deferred_lighting.wesl. Kept:
// world-pos reconstruction from reversed-Z depth, octahedron normal decode, the
// sun directional term (EON retro diffuse + GGX spec), the SH ambient diffuse
// term, (task S2.B2) the IBL ambient-specular split-sum term, and (task M6)
// screen-space GTAO (binding 7): final AO = min(gtao, baked-AO). STILL REMOVED
// (return in later tasks): shadow-map PCF (binding 5/6), contact shadows (8) —
// hardcoded shadow=1.
//
// M6 group-0 layout: frame UBO @0, gbuffer depth @1, normals @2, albedo @3,
// material @4 (all textureLoad'd), GTAO ao @7, prefiltered env cube @9 + its
// sampler @10, BRDF split-sum LUT @11 + its sampler @12. Bindings 5/6/8 stay
// unused (WGSL bindings need not be contiguous). The SceneRenderer ALWAYS binds
// @7 (a GTAO texture, or a 1.0-cleared texture when GTAO is off) and @9..@12
// (prefiltered cube or a 1x1 black fallback cube + the BRDF LUT), so this
// layout is always satisfiable.
import package::common::frame::{frame, getFullscreenTriangleVertex, reconstructLinearZ, reconstructViewSpacePosition, viewSpaceToWorldOffsetedSpacePosition, decodeOctahedron};
import package::common::color::srgb_to_linear;
import package::common::sh_lighting::evaluateAmbientSHL2;
import package::common::brdf::{evaluateEonRetroBrdf, evaluateSpecularGgxBrdf, evaluateLagardeFresnel};

const F0_DIELECTRIC: vec3<f32> = vec3<f32>(0.04);

@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var normals_texture: texture_2d<f32>;
@group(0) @binding(3) var albedo_texture: texture_2d<f32>;
@group(0) @binding(4) var material_texture: texture_2d<f32>;  // RGBA8: R=roughness, G=shadow, B=baked AO
@group(0) @binding(7) var ao_texture: texture_2d<f32>;          // GTAO screen-space AO (R8: 1=unoccluded)
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

// Computes Specular Occlusion (ported from sampo's deferred_lighting.wesl).
// Prevents ambient specular 'glow' from leaking into deep cracks (AO areas):
// scales the AO influence by roughness and the viewing angle so sharp
// reflections (low roughness) are occluded more strictly.
fn computeSpecularOcclusion(NdotV: f32, ao: f32, roughness: f32) -> f32 {
    return clamp(pow(NdotV + ao, roughness) - 1.0 + ao, 0.0, 1.0);
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);
    let depth = textureLoad(depth_texture, texCoord, 0);
    let linearZ = reconstructLinearZ(depth);

    // Sky / no-geometry: leave the pass's clear color untouched.
    if (linearZ >= frame.farPlane * 0.999) {
        discard;
    }

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

    // 3. Material properties from GBuffer (R=roughness)
    let materialData = textureLoad(material_texture, texCoord, 0);
    let roughness = materialData.r;

    // 4. Shadowing / occlusion — no shadow map or contact shadows yet (B1);
    // final AO = min(screen-space GTAO, baked AO). `enableGtao` gates whether
    // the GTAO compute pass ran this frame: when it didn't, gtao=1.0 so
    // ao = min(1.0, baked) = baked (M4 behaviour).
    let shadow = 1.0;
    let gtao = select(1.0, textureLoad(ao_texture, texCoord, 0).r, frame.enableGtao != 0u);
    let ao = min(gtao, materialData.b);

    // 5. Lighting calculation
    let albedoLinear = srgb_to_linear(textureLoad(albedo_texture, texCoord, 0).rgb);
    let NdotL = max(dot(normal, lightDir), 0.0);

    // Direct lighting (sun): EON diffuse (includes NdotL internally) + GGX spec
    let diffuseTerm = evaluateEonRetroBrdf(normal, lightDir, viewDir, roughness);
    let specularTerm = evaluateSpecularGgxBrdf(normal, viewDir, lightDir, roughness, F0_DIELECTRIC);
    let directLighting = (diffuseTerm * albedoLinear + specularTerm) * shadow * NdotL * frame.sunColor.xyz;

    // Ambient lighting: SH L2 diffuse + IBL split-sum specular.
    let ambientDiffuse = evaluateAmbientSHL2(normal, frame.ambientSH) * ao;

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

    // Roughness-aware indirect Fresnel to keep energy in check: the diffuse
    // term is dimmed where the specular term picks up (Lagarde approximation).
    let indirectFresnel = evaluateLagardeFresnel(NdotV, roughness, F0_DIELECTRIC);
    let diffuseWeight = 1.0 - indirectFresnel;

    let indirectLighting = (ambientDiffuse * albedoLinear * diffuseWeight) + ambientSpecular;

    let finalColorLinear = directLighting + indirectLighting;

    // Directional-shadow debug visualization (T1 plumbing; see
    // ShadowDebugMode in scene_renderer.hpp). Uniform branch (frame.debugFlags
    // is the same for every pixel this frame), so this does not diverge
    // per-fragment. At T1 `shadow` is hardcoded 1.0 above, so any nonzero
    // mode renders all-white — that's the expected "plumbing works" state.
    // T3 replaces the hardcoded `shadow` with the real
    // min(shadowMapPCF, contactShadow) term and expands this branch to
    // select per-mode (1=combined, 2=map-only, 3=contact-only).
    if (frame.debugFlags != 0u) {
        return vec4<f32>(shadow, shadow, shadow, 1.0);
    }

    return vec4<f32>(finalColorLinear, 1.0);
}
