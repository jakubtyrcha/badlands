// BRDF functions for deferred lighting.
//
// Ported verbatim from sampo's src/shaders/common/brdf.wesl.

const FRESNEL_EXPONENT: f32 = 5.0; // Controls the falloff 'sharpness' of the edge-lighting

/**
 * Evaluates the composite Diffuse BRDF.
 * @param N Surface Normal
 * @param L Light Direction (to sun)
 * @param V View Direction (to camera)
 * @param roughness Material roughness [0-1]
 */
fn evaluateEonRetroBrdf(N: vec3<f32>, L: vec3<f32>, V: vec3<f32>, roughness: f32) -> f32 {
    let NdotL = max(dot(N, L), 0.0);
    let NdotV = max(dot(N, V), 0.0);
    let H = normalize(L + V);
    let LdotH = max(dot(L, H), 0.0);

    // --- Part 1: EON (Energy-preserving Oren-Nayar) ---
    // Derived from Portsmouth et al. (2024)
    let sigma2 = roughness * roughness;

    // Qualitative Oren-Nayar coefficients
    let A = 1.0 - 0.5 * (sigma2 / (sigma2 + 0.33));
    let B = 0.45 * (sigma2 / (sigma2 + 0.09));

    // Azimuthal difference approximation via projection
    let L_proj = normalize(L - N * NdotL);
    let V_proj = normalize(V - N * NdotV);
    let cos_phi_diff = max(0.0, dot(L_proj, V_proj));

    let sin_alpha = sqrt(max(0.0, 1.0 - min(NdotL, NdotV) * min(NdotL, NdotV)));
    let tan_beta = sqrt(max(0.0, 1.0 - max(NdotL, NdotV) * max(NdotL, NdotV))) / max(max(NdotL, NdotV), 1e-6);

    let basic_on = (A + B * cos_phi_diff * sin_alpha * tan_beta);

    // Analytic energy compensation prevents darkening at grazing angles
    let energy_compensation = 1.0 + roughness * (1.0 - NdotL) * (1.0 - NdotV) * 0.5;

    var final_diffuse = basic_on * energy_compensation;

    // --- Part 2: Callisto-Style Retroreflection (Diffuse Fresnel) ---
    // Mimics the 'Pragmatic BRDF' used in The Callisto Protocol
    let fresnel_l = pow(1.0 - NdotL, FRESNEL_EXPONENT);
    let fresnel_v = pow(1.0 - NdotV, FRESNEL_EXPONENT);

    // Retroreflection: Brightens the surface when light and view align at edges
    let retro = roughness * (fresnel_l + fresnel_v + fresnel_l * fresnel_v * (LdotH * LdotH * 2.0 - 1.0));

    return final_diffuse + retro;
}

/**
 * Standard GGX Specular Lobe (Cook-Torrance)
 * Provides the sharp highlights and Fresnel edge-pop.
 */
fn evaluateSpecularGgxBrdf(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32, F0: vec3<f32>) -> vec3<f32> {
    let H = normalize(V + L);
    let dotNH = max(dot(N, H), 0.0);
    let dotNV = max(dot(N, V), 0.0);
    let dotNL = max(dot(N, L), 0.0);
    let dotLH = max(dot(L, H), 0.0);

    let alpha = roughness * roughness;
    let alpha2 = alpha * alpha;

    // D: Normal Distribution Function (GGX)
    let denom = (dotNH * dotNH * (alpha2 - 1.0) + 1.0);
    let D = alpha2 / (3.14159 * denom * denom);

    // F: Fresnel (Schlick) - This is the primary 'edge pop' source
    let F = F0 + (1.0 - F0) * pow(1.0 - dotLH, FRESNEL_EXPONENT);

    // V: Visibility / Geometry (Smith Joint)
    let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    let vis = 1.0 / ( (dotNV * (1.0 - k) + k) * (dotNL * (1.0 - k) + k) * 4.0 + 1e-5 );

    return D * F * vis;
}

// ============================================================================
// INDIRECT FRESNEL (Lagarde Approximation)
// ============================================================================
// Fixes the 'mega-fresnel' look by accounting for how roughness
// dampens the Fresnel peak at grazing angles.
// ============================================================================
fn evaluateLagardeFresnel(NdotV: f32, roughness: f32, F0: vec3<f32>) -> vec3<f32> {
    // Lagarde's approximation for Environment BRDF
    // It smoothly transitions from F0 to a roughness-limited maximum
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, FRESNEL_EXPONENT);
}
