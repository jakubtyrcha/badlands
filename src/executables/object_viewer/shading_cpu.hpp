#pragma once

// The CPU mirror of shaders/slang/common/{brdf,sh_lighting,standard_lighting}.slang.
//
// EXISTS AS AN ORACLE, and it is the ONLY check that the WESL -> Slang port did
// not quietly change the shading. Every other assertion in this stage would
// pass just as well against a Lambert term or a dropped retroreflection lobe:
// they check that a surface was lit, not that it was lit by these equations.
//
// So this is deliberately a THIRD transcription of the same maths, made from
// shaders/common/brdf.wesl rather than from the Slang. Two independent ports
// agreeing is evidence; a port compared against itself is not.
//
// Header-only and glm-only, so it needs no device and no target.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace badlands::object_viewer::cpu {

inline constexpr float kFresnelExponent = 5.0f;
inline const glm::vec3 kF0Dielectric{0.04f};

inline float EvaluateEonRetroBrdf(glm::vec3 N, glm::vec3 L, glm::vec3 V,
                                  float roughness) {
  const float NdotL = std::max(glm::dot(N, L), 0.0f);
  const float NdotV = std::max(glm::dot(N, V), 0.0f);
  const glm::vec3 H = glm::normalize(L + V);
  const float LdotH = std::max(glm::dot(L, H), 0.0f);

  const float sigma2 = roughness * roughness;
  const float A = 1.0f - 0.5f * (sigma2 / (sigma2 + 0.33f));
  const float B = 0.45f * (sigma2 / (sigma2 + 0.09f));

  const glm::vec3 L_proj = glm::normalize(L - N * NdotL);
  const glm::vec3 V_proj = glm::normalize(V - N * NdotV);
  const float cos_phi_diff = std::max(0.0f, glm::dot(L_proj, V_proj));

  const float mn = std::min(NdotL, NdotV);
  const float mx = std::max(NdotL, NdotV);
  const float sin_alpha = std::sqrt(std::max(0.0f, 1.0f - mn * mn));
  const float tan_beta =
      std::sqrt(std::max(0.0f, 1.0f - mx * mx)) / std::max(mx, 1e-6f);

  const float basic_on = A + B * cos_phi_diff * sin_alpha * tan_beta;
  const float energy_compensation =
      1.0f + roughness * (1.0f - NdotL) * (1.0f - NdotV) * 0.5f;
  const float final_diffuse = basic_on * energy_compensation;

  const float fresnel_l = std::pow(1.0f - NdotL, kFresnelExponent);
  const float fresnel_v = std::pow(1.0f - NdotV, kFresnelExponent);
  const float retro =
      roughness * (fresnel_l + fresnel_v +
                   fresnel_l * fresnel_v * (LdotH * LdotH * 2.0f - 1.0f));
  return final_diffuse + retro;
}

inline glm::vec3 EvaluateSpecularGgxBrdf(glm::vec3 N, glm::vec3 V, glm::vec3 L,
                                         float roughness, glm::vec3 F0) {
  const glm::vec3 H = glm::normalize(V + L);
  const float dotNH = std::max(glm::dot(N, H), 0.0f);
  const float dotNV = std::max(glm::dot(N, V), 0.0f);
  const float dotNL = std::max(glm::dot(N, L), 0.0f);
  const float dotLH = std::max(glm::dot(L, H), 0.0f);

  const float alpha = roughness * roughness;
  const float alpha2 = alpha * alpha;
  const float denom = dotNH * dotNH * (alpha2 - 1.0f) + 1.0f;
  const float D = alpha2 / (3.14159f * denom * denom);
  const glm::vec3 F =
      F0 + (glm::vec3(1.0f) - F0) * std::pow(1.0f - dotLH, kFresnelExponent);
  const float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
  const float vis =
      1.0f / ((dotNV * (1.0f - k) + k) * (dotNL * (1.0f - k) + k) * 4.0f +
              1e-5f);
  return D * F * vis;
}

inline glm::vec3 EvaluateLagardeFresnel(float NdotV, float roughness,
                                        glm::vec3 F0) {
  return F0 + (glm::max(glm::vec3(1.0f - roughness), F0) - F0) *
                  std::pow(1.0f - NdotV, kFresnelExponent);
}

// The ENGINE convention: the diffuse convolution is baked into the
// coefficients at projection time, so this evaluates the raw basis. Applying
// the c1..c5 constants here as well is the double-convolution trap named in
// sh_lighting.slang.
inline glm::vec3 EvaluateAmbientSHL2(glm::vec3 n, const glm::vec4 sh[9]) {
  return glm::vec3(sh[0]) + glm::vec3(sh[1]) * n.y + glm::vec3(sh[2]) * n.z +
         glm::vec3(sh[3]) * n.x + glm::vec3(sh[4]) * (n.x * n.y) +
         glm::vec3(sh[5]) * (n.y * n.z) +
         glm::vec3(sh[6]) * (3.0f * n.z * n.z - 1.0f) +
         glm::vec3(sh[7]) * (n.x * n.z) +
         glm::vec3(sh[8]) * (n.x * n.x - n.y * n.y);
}

// The patched inputs: shadow = 1, ambientSpecular = 0. Kept as parameters so
// this matches ShadeStandard's signature and a later stage filling them in
// needs no change here.
inline glm::vec3 ShadeStandard(glm::vec3 albedoLinear, glm::vec3 N, glm::vec3 V,
                               float roughness, float ao, float metallic,
                               float shadow, glm::vec3 ambientSpecular,
                               glm::vec3 sunDirection, glm::vec3 sunColor,
                               const glm::vec4 ambientSH[9]) {
  const glm::vec3 L = glm::normalize(sunDirection);
  const float NdotV = std::max(glm::dot(N, V), 0.0f);
  const float NdotL = std::max(glm::dot(N, L), 0.0f);

  // The metallic workflow, mirroring the Slang. metallic = 0 reproduces the
  // WESL original exactly, which is what keeps this a port proof for every
  // dielectric surface even though the metal path is a divergence.
  const glm::vec3 F0 = glm::mix(kF0Dielectric, albedoLinear, metallic);
  const glm::vec3 diffuseAlbedo = albedoLinear * (1.0f - metallic);

  const float diffuseTerm = EvaluateEonRetroBrdf(N, L, V, roughness);
  const glm::vec3 specularTerm =
      EvaluateSpecularGgxBrdf(N, V, L, roughness, F0);
  const glm::vec3 directLighting =
      (diffuseTerm * diffuseAlbedo + specularTerm) * shadow * NdotL * sunColor;

  const glm::vec3 ambientDiffuse = EvaluateAmbientSHL2(N, ambientSH) * ao;
  const glm::vec3 indirectFresnel =
      EvaluateLagardeFresnel(NdotV, roughness, F0);
  const glm::vec3 diffuseWeight = glm::vec3(1.0f) - indirectFresnel;
  const glm::vec3 indirectLighting =
      (ambientDiffuse * diffuseAlbedo * diffuseWeight) + ambientSpecular;

  return directLighting + indirectLighting;
}

}  // namespace badlands::object_viewer::cpu
