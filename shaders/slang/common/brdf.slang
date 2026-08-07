// BRDF functions for deferred lighting.
//
// PORTED VERBATIM from shaders/common/brdf.wesl (itself ported from sampo).
// The maths is unchanged line for line -- only the syntax moved. If this ever
// disagrees with the WESL original, the WESL is right and this is the bug: the
// `lit` oracle in object_viewer's headless run is a CPU port of the SAME
// equations, and it exists precisely to notice a port that drifted.
//
// NOT REWRITTEN AS TEXTBOOK GGX, deliberately. The diffuse term here is EON
// (energy-preserving Oren-Nayar) plus Callisto-style retroreflection, which is
// what the engine's surfaces are authored against; substituting Lambert would
// make object_viewer show something the game does not.
module brdf;

// Controls the falloff 'sharpness' of the edge-lighting.
public static const float kFresnelExponent = 5.0;

// Evaluates the composite Diffuse BRDF.
//   N  surface normal
//   L  light direction (toward the sun)
//   V  view direction (toward the camera)
public float EvaluateEonRetroBrdf(float3 N, float3 L, float3 V, float roughness)
{
    let NdotL = max(dot(N, L), 0.0);
    let NdotV = max(dot(N, V), 0.0);
    let H = normalize(L + V);
    let LdotH = max(dot(L, H), 0.0);

    // --- Part 1: EON (Energy-preserving Oren-Nayar), Portsmouth et al. (2024)
    let sigma2 = roughness * roughness;
    let A = 1.0 - 0.5 * (sigma2 / (sigma2 + 0.33));
    let B = 0.45 * (sigma2 / (sigma2 + 0.09));

    // Azimuthal difference approximation via projection.
    let L_proj = normalize(L - N * NdotL);
    let V_proj = normalize(V - N * NdotV);
    let cos_phi_diff = max(0.0, dot(L_proj, V_proj));

    let sin_alpha = sqrt(max(0.0, 1.0 - min(NdotL, NdotV) * min(NdotL, NdotV)));
    let tan_beta = sqrt(max(0.0, 1.0 - max(NdotL, NdotV) * max(NdotL, NdotV))) /
                   max(max(NdotL, NdotV), 1e-6);

    let basic_on = (A + B * cos_phi_diff * sin_alpha * tan_beta);

    // Analytic energy compensation prevents darkening at grazing angles.
    let energy_compensation =
        1.0 + roughness * (1.0 - NdotL) * (1.0 - NdotV) * 0.5;
    let final_diffuse = basic_on * energy_compensation;

    // --- Part 2: Callisto-style retroreflection (diffuse Fresnel)
    let fresnel_l = pow(1.0 - NdotL, kFresnelExponent);
    let fresnel_v = pow(1.0 - NdotV, kFresnelExponent);
    let retro = roughness * (fresnel_l + fresnel_v +
                             fresnel_l * fresnel_v * (LdotH * LdotH * 2.0 - 1.0));

    return final_diffuse + retro;
}

// Standard GGX specular lobe (Cook-Torrance).
public float3 EvaluateSpecularGgxBrdf(float3 N, float3 V, float3 L,
                                      float roughness, float3 F0)
{
    let H = normalize(V + L);
    let dotNH = max(dot(N, H), 0.0);
    let dotNV = max(dot(N, V), 0.0);
    let dotNL = max(dot(N, L), 0.0);
    let dotLH = max(dot(L, H), 0.0);

    let alpha = roughness * roughness;
    let alpha2 = alpha * alpha;

    // D: normal distribution (GGX)
    let denom = (dotNH * dotNH * (alpha2 - 1.0) + 1.0);
    let D = alpha2 / (3.14159 * denom * denom);

    // F: Fresnel (Schlick) -- the primary 'edge pop' source
    let F = F0 + (1.0 - F0) * pow(1.0 - dotLH, kFresnelExponent);

    // V: visibility / geometry (Smith joint)
    let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    let vis = 1.0 / ((dotNV * (1.0 - k) + k) * (dotNL * (1.0 - k) + k) * 4.0 +
                     1e-5);

    return D * F * vis;
}

// Indirect Fresnel (Lagarde approximation). Accounts for how roughness dampens
// the Fresnel peak at grazing angles, which is what stops the 'mega-fresnel'
// look.
public float3 EvaluateLagardeFresnel(float NdotV, float roughness, float3 F0)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) *
                    pow(1.0 - NdotV, kFresnelExponent);
}
