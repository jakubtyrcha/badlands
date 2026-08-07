// Shared "standard" surface shading: sun directional (EON retro diffuse + GGX
// specular) + SH ambient diffuse + caller-supplied IBL ambient specular.
//
// PORTED VERBATIM from shaders/common/standard_lighting.wesl. Only the INPUTS
// changed, and only in how they arrive: the WESL reads `frame` from the group-0
// UBO, this takes sun direction and colour as arguments so object_viewer can
// drive them from its own uniform. The equation is untouched.
//
// ONE DELIBERATE DIVERGENCE FROM THE ORIGINAL: METALLIC.
//
// standard_lighting.wesl fixes F0 at 0.04 -- dielectric -- and never reads a
// metallic channel; material_library.hpp says the same on the CPU side
// ("metallic is currently unused"). That made object_viewer's Metallic debug
// view show an authored channel that nothing consumed, so the metallic workflow
// was added here on request:
//
//   F0      = lerp(0.04, albedo, metallic)   -- metals tint their specular
//   diffuse *= (1 - metallic)                -- and have no diffuse lobe
//
// This is the standard metallic workflow and it is energy-conserving, but it
// means THIS FILE NO LONGER MATCHES ITS WESL ORIGINAL. Porting it back to
// shaders/common/standard_lighting.wesl would change how badlands_game renders
// every surface whose ARM blue channel is non-zero, which is a separate
// decision with its own approval.
//
// STILL NOT DONE, because these would be changing the maths rather than filling
// in an input: shadows and IBL. Callers pass shadow = 1 and ambientSpecular = 0,
// and the parameters stay so both can be filled in without touching this file.
module standard_lighting;

import brdf;
import sh_lighting;

public static const float3 kF0Dielectric = float3(0.04);

// Roughness/view-angle-aware specular occlusion (prevents ambient specular
// leaking into AO cracks).
public float ComputeSpecularOcclusion(float NdotV, float ao, float roughness)
{
    return clamp(pow(NdotV + ao, roughness) - 1.0 + ao, 0.0, 1.0);
}

// Standard lit surface colour, LINEAR. `ambientSpecular` is the IBL split-sum
// term, sampled by the caller from its own bound prefiltered-env + BRDF LUT.
public float3 ShadeStandard(float3 albedoLinear, float3 N, float3 V,
                            float roughness, float ao, float metallic,
                            float shadow, float3 ambientSpecular,
                            float3 sunDirection, float3 sunColor,
                            float4 ambientSH[9])
{
    let L = normalize(sunDirection);
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);

    // The metallic workflow. A metal's specular takes the albedo's tint and it
    // has no diffuse lobe at all; a dielectric keeps F0 at 0.04 and its full
    // diffuse. Both fall out of the same two lerps, so metallic = 0 reproduces
    // the WESL original exactly -- which is what keeps every dielectric surface
    // rendering as it did before this was added.
    let F0 = lerp(kF0Dielectric, albedoLinear, metallic);
    let diffuseAlbedo = albedoLinear * (1.0 - metallic);

    // Direct sun: EON retro diffuse + GGX specular, shadowed.
    let diffuseTerm = EvaluateEonRetroBrdf(N, L, V, roughness);
    let specularTerm = EvaluateSpecularGgxBrdf(N, V, L, roughness, F0);
    let directLighting =
        (diffuseTerm * diffuseAlbedo + specularTerm) * shadow * NdotL * sunColor;

    // Ambient: SH L2 diffuse (roughness-aware indirect Fresnel weighting) + IBL.
    let ambientDiffuse = EvaluateAmbientSHL2(N, ambientSH) * ao;
    let indirectFresnel = EvaluateLagardeFresnel(NdotV, roughness, F0);
    let diffuseWeight = 1.0 - indirectFresnel;
    let indirectLighting =
        (ambientDiffuse * diffuseAlbedo * diffuseWeight) + ambientSpecular;

    return directLighting + indirectLighting;
}
