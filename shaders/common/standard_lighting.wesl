// Shared "standard" surface shading: sun directional (EON retro diffuse + GGX
// specular) + SH ambient diffuse + (caller-supplied) IBL ambient specular. Used
// by BOTH the deferred lighting pass and forward-opaque materials so forward
// shading matches the G-buffer by construction.
import package::common::frame::{frame};
import package::common::sh_lighting::evaluateAmbientSHL2;
import package::common::brdf::{evaluateEonRetroBrdf, evaluateSpecularGgxBrdf, evaluateLagardeFresnel};

const F0_DIELECTRIC: vec3<f32> = vec3<f32>(0.04);

// Roughness/view-angle-aware specular occlusion (prevents ambient specular
// leaking into AO cracks). Moved verbatim from deferred_lighting.wesl.
fn computeSpecularOcclusion(NdotV: f32, ao: f32, roughness: f32) -> f32 {
    return clamp(pow(NdotV + ao, roughness) - 1.0 + ao, 0.0, 1.0);
}

// Standard lit surface color (linear). `ambientSpecular` is the IBL split-sum
// term, sampled by the caller from its own bound prefiltered-env + BRDF LUT.
fn shadeStandard(albedoLinear: vec3<f32>, N: vec3<f32>, V: vec3<f32>,
                 roughness: f32, ao: f32, shadow: f32,
                 ambientSpecular: vec3<f32>) -> vec3<f32> {
    let L = normalize(frame.sunDirection.xyz);
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);

    // Direct sun: EON retro diffuse + GGX specular, shadowed.
    let diffuseTerm = evaluateEonRetroBrdf(N, L, V, roughness);
    let specularTerm = evaluateSpecularGgxBrdf(N, V, L, roughness, F0_DIELECTRIC);
    let directLighting = (diffuseTerm * albedoLinear + specularTerm) * shadow * NdotL * frame.sunColor.xyz;

    // Ambient: SH L2 diffuse (roughness-aware indirect Fresnel weighting) + IBL specular.
    let ambientDiffuse = evaluateAmbientSHL2(N, frame.ambientSH) * ao;
    let indirectFresnel = evaluateLagardeFresnel(NdotV, roughness, F0_DIELECTRIC);
    let diffuseWeight = 1.0 - indirectFresnel;
    let indirectLighting = (ambientDiffuse * albedoLinear * diffuseWeight) + ambientSpecular;

    return directLighting + indirectLighting;
}

const kPhasePower: f32 = 4.0;  // forward-scatter lobe sharpness (flare)

// Thin-slab diffuse transmission (subsurface look): light entering the far side
// of a thin translucent surface and exiting toward the viewer. Split like
// shadeStandard's reflection — a DIRECT sun term (caller gates it with the
// shadow map: "shadow is for the directional light") plus an AMBIENT term from
// the environment on the BACK hemisphere (caller gates it with ao: "AO/IBL for
// the glare"). transmissionColor (T) and strength (s) are the material's
// transmittance; reflectance and transmittance are independent surface
// properties, so this is ADDED to the reflected color with no energy rebalancing.
fn evaluateTranslucency(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>,
                        shadow: f32, ao: f32,
                        transmissionColor: vec3<f32>, strength: f32) -> vec3<f32> {
    let backLit = max(-dot(N, L), 0.0);                       // sun behind the surface
    let flare = pow(max(dot(V, -L), 0.0), kPhasePower);       // forward-scatter peak
    let direct = shadow * frame.sunColor.xyz * backLit * (1.0 + flare);
    let ambient = ao * evaluateAmbientSHL2(-N, frame.ambientSH);
    return strength * transmissionColor * (direct + ambient);
}
