#pragma once

// Ported from sampo's src/core/math/spherical_harmonics.hpp, namespace
// sampo -> badlands (otherwise verbatim: header-only, only depends on glm).
//
// Spherical Harmonics for diffuse ambient lighting.
// L1: 4 coefficients (1 constant + 3 linear)
// L2: 9 coefficients (L0 + L1 + 5 quadratic)
//
// The coefficient conventions here (raw polynomials, NOT orthonormal basis,
// diffuse convolution folded into the directional-projection helpers) match
// the shader-side evaluateAmbientSHL2() in shaders/common/sh_lighting.wesl,
// so a set of coefficients produced here can be uploaded to the frame UBO's
// ambientSH and evaluated directly.

#include <array>
#include <cmath>
#include <glm/glm.hpp>

namespace badlands {

namespace sh {

// SH L1 coefficients: [L0, L1_y, L1_z, L1_x]
using SHL1 = std::array<glm::vec3, 4>;

// SH L2 coefficients: [L0, L1_y, L1_z, L1_x, L2_xy, L2_yz, L2_0, L2_xz,
// L2_x2y2]
using SHL2 = std::array<glm::vec3, 9>;

// Project a directional light into SH L1 coefficients
// The result includes diffuse convolution factors
inline SHL1 ProjectDirectionalLight(glm::vec3 direction, glm::vec3 color) {
  // Pre-computed constants including diffuse convolution
  constexpr float kC0 = 0.886227f;  // pi * 0.282095
  constexpr float kC1 = 1.023328f;  // (2*pi/3) * 0.488603

  return {{
      color * kC0,
      color * (kC1 * direction.y),
      color * (kC1 * direction.z),
      color * (kC1 * direction.x),
  }};
}

// Create hemisphere ambient from sky and ground colors
// Sky color applies to normals facing up (+Y), ground to normals facing down
// (-Y)
inline SHL1 CreateHemisphereAmbient(glm::vec3 sky_color,
                                    glm::vec3 ground_color) {
  auto sky = ProjectDirectionalLight(glm::vec3(0.0f, 1.0f, 0.0f), sky_color);
  auto ground =
      ProjectDirectionalLight(glm::vec3(0.0f, -1.0f, 0.0f), ground_color);

  return {{
      sky[0] + ground[0],
      sky[1] + ground[1],
      sky[2] + ground[2],
      sky[3] + ground[3],
  }};
}

// Add two SH L1 representations
inline SHL1 Add(const SHL1& a, const SHL1& b) {
  return {{
      a[0] + b[0],
      a[1] + b[1],
      a[2] + b[2],
      a[3] + b[3],
  }};
}

// Scale SH L1 coefficients
inline SHL1 Scale(const SHL1& sh, float scale) {
  return {{
      sh[0] * scale,
      sh[1] * scale,
      sh[2] * scale,
      sh[3] * scale,
  }};
}

// Create flat ambient (constant in all directions)
inline SHL1 CreateFlatAmbient(glm::vec3 color) {
  return {{color, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)}};
}

// ============================================================================
// SH Basis Functions (raw, without diffuse convolution)
// ============================================================================

constexpr float kY00 = 0.282095f;
constexpr float kY1m = 0.488603f;

// Evaluate raw SH L1 basis functions at a direction
inline std::array<float, 4> EvaluateSHL1Basis(glm::vec3 dir) {
  return {{
      kY00,
      kY1m * dir.y,
      kY1m * dir.z,
      kY1m * dir.x,
  }};
}

// ============================================================================
// Diffuse Convolution Factors
// ============================================================================

constexpr float kDiffuseA0 = 3.141593f;  // pi
constexpr float kDiffuseA1 = 2.094395f;  // 2*pi/3

// Apply diffuse convolution to raw SH coefficients
inline SHL1 ApplyDiffuseConvolution(const SHL1& raw_sh) {
  return {{
      raw_sh[0] * kDiffuseA0,
      raw_sh[1] * kDiffuseA1,
      raw_sh[2] * kDiffuseA1,
      raw_sh[3] * kDiffuseA1,
  }};
}

// ============================================================================
// Arbitrary Function Projection
// ============================================================================

// Project an arbitrary radiance function to SH L1 using Monte Carlo
// integration. The function receives a direction and returns RGB radiance.
template <typename RadianceFunc>
SHL1 ProjectFunctionToSHL1(RadianceFunc&& func, int sample_count) {
  SHL1 result = {
      {glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)}};

  const float golden_ratio = (1.0f + std::sqrt(5.0f)) / 2.0f;
  const float golden_angle = 2.0f * 3.14159265f / (golden_ratio * golden_ratio);

  for (int i = 0; i < sample_count; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sample_count);
    float inclination = std::acos(1.0f - 2.0f * t);
    float azimuth = golden_angle * static_cast<float>(i);

    float sin_inc = std::sin(inclination);
    glm::vec3 dir{sin_inc * std::cos(azimuth),
                  std::cos(inclination),  // Y-up
                  sin_inc * std::sin(azimuth)};

    glm::vec3 radiance = func(dir);

    result[0] += radiance;          // L0: 1
    result[1] += radiance * dir.y;  // L1: y
    result[2] += radiance * dir.z;  // L1: z
    result[3] += radiance * dir.x;  // L1: x
  }

  float weight = 1.0f / static_cast<float>(sample_count);
  return Scale(result, weight);
}

// ============================================================================
// SH Evaluation (CPU-side, for testing)
// ============================================================================

inline glm::vec3 EvaluateSHL1(const SHL1& sh, glm::vec3 normal) {
  return sh[0] + sh[1] * normal.y + sh[2] * normal.z + sh[3] * normal.x;
}

// ============================================================================
// SH L2 Operations
// ============================================================================

constexpr float kY2m2 = 1.092548f;  // sqrt(15/pi) / 2
constexpr float kY2m1 = 1.092548f;  // sqrt(15/pi) / 2
constexpr float kY20 = 0.315392f;   // sqrt(5/pi) / 4
constexpr float kY21 = 1.092548f;   // sqrt(15/pi) / 2
constexpr float kY22 = 0.546274f;   // sqrt(15/pi) / 4

constexpr float kDiffuseA2 = 0.785398f;  // pi/4

// Evaluate raw SH L2 basis functions at a direction
inline std::array<float, 9> EvaluateSHL2Basis(glm::vec3 dir) {
  float x = dir.x, y = dir.y, z = dir.z;
  return {{
      kY00,
      kY1m * y,
      kY1m * z,
      kY1m * x,
      kY2m2 * x * y,
      kY2m1 * y * z,
      kY20 * (3.0f * z * z - 1.0f),
      kY21 * x * z,
      kY22 * (x * x - y * y),
  }};
}

// Project a directional light into SH L2 coefficients (incl. diffuse convolution)
inline SHL2 ProjectDirectionalLightL2(glm::vec3 direction, glm::vec3 color) {
  constexpr float kC0 = 0.886227f;       // pi * 0.282095
  constexpr float kC1 = 1.023328f;       // (2*pi/3) * 0.488603
  constexpr float kC2_xy = 0.858086f;    // (pi/4) * 1.092548
  constexpr float kC2_yz = 0.858086f;    // (pi/4) * 1.092548
  constexpr float kC2_0 = 0.247708f;     // (pi/4) * 0.315392
  constexpr float kC2_xz = 0.858086f;    // (pi/4) * 1.092548
  constexpr float kC2_x2y2 = 0.429043f;  // (pi/4) * 0.546274

  float x = direction.x, y = direction.y, z = direction.z;

  return {{
      color * kC0,
      color * (kC1 * y),
      color * (kC1 * z),
      color * (kC1 * x),
      color * (kC2_xy * x * y),
      color * (kC2_yz * y * z),
      color * (kC2_0 * (3.0f * z * z - 1.0f)),
      color * (kC2_xz * x * z),
      color * (kC2_x2y2 * (x * x - y * y)),
  }};
}

// Add two SH L2 representations
inline SHL2 AddL2(const SHL2& a, const SHL2& b) {
  SHL2 result;
  for (int i = 0; i < 9; ++i) {
    result[i] = a[i] + b[i];
  }
  return result;
}

// Scale SH L2 coefficients
inline SHL2 ScaleL2(const SHL2& sh, float scale) {
  SHL2 result;
  for (int i = 0; i < 9; ++i) {
    result[i] = sh[i] * scale;
  }
  return result;
}

// Create flat ambient (constant in all directions) as L2
inline SHL2 CreateFlatAmbientL2(glm::vec3 color) {
  SHL2 result{};
  result[0] = color;
  return result;
}

// Create hemisphere ambient from sky and ground colors (L2)
inline SHL2 CreateHemisphereAmbientL2(glm::vec3 sky_color,
                                      glm::vec3 ground_color) {
  auto sky = ProjectDirectionalLightL2(glm::vec3(0.0f, 1.0f, 0.0f), sky_color);
  auto ground =
      ProjectDirectionalLightL2(glm::vec3(0.0f, -1.0f, 0.0f), ground_color);
  return AddL2(sky, ground);
}

// Create 6-direction ambient for more colorful directional lighting
inline SHL2 Create6DirectionAmbient(glm::vec3 pos_x_color, glm::vec3 neg_x_color,
                                    glm::vec3 pos_y_color, glm::vec3 neg_y_color,
                                    glm::vec3 pos_z_color,
                                    glm::vec3 neg_z_color) {
  SHL2 result{};
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(1.0f, 0.0f, 0.0f),
                                                   pos_x_color));
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(-1.0f, 0.0f, 0.0f),
                                                   neg_x_color));
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(0.0f, 1.0f, 0.0f),
                                                   pos_y_color));
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(0.0f, -1.0f, 0.0f),
                                                   neg_y_color));
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(0.0f, 0.0f, 1.0f),
                                                   pos_z_color));
  result = AddL2(result, ProjectDirectionalLightL2(glm::vec3(0.0f, 0.0f, -1.0f),
                                                   neg_z_color));
  return result;
}

// Apply diffuse convolution to raw SH L2 coefficients
inline SHL2 ApplyDiffuseConvolutionL2(const SHL2& raw_sh) {
  return {{
      raw_sh[0] * kDiffuseA0,
      raw_sh[1] * kDiffuseA1,
      raw_sh[2] * kDiffuseA1,
      raw_sh[3] * kDiffuseA1,
      raw_sh[4] * kDiffuseA2,
      raw_sh[5] * kDiffuseA2,
      raw_sh[6] * kDiffuseA2,
      raw_sh[7] * kDiffuseA2,
      raw_sh[8] * kDiffuseA2,
  }};
}

// Project an arbitrary radiance function to SH L2 using Monte Carlo
// integration (Fibonacci-lattice uniform sphere sampling). The function
// receives a direction and returns RGB radiance. Returns coefficients matching
// the shader evaluation convention (sh[0] = average color for a constant func).
template <typename RadianceFunc>
SHL2 ProjectFunctionToSHL2(RadianceFunc&& func, int sample_count) {
  SHL2 result{};

  const float golden_ratio = (1.0f + std::sqrt(5.0f)) / 2.0f;
  const float golden_angle = 2.0f * 3.14159265f / (golden_ratio * golden_ratio);

  for (int i = 0; i < sample_count; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sample_count);
    float inclination = std::acos(1.0f - 2.0f * t);
    float azimuth = golden_angle * static_cast<float>(i);

    float sin_inc = std::sin(inclination);
    glm::vec3 dir{sin_inc * std::cos(azimuth),
                  std::cos(inclination),  // Y-up
                  sin_inc * std::sin(azimuth)};

    glm::vec3 radiance = func(dir);

    float x = dir.x, y = dir.y, z = dir.z;
    result[0] += radiance;                          // L0: 1
    result[1] += radiance * y;                      // L1: y
    result[2] += radiance * z;                      // L1: z
    result[3] += radiance * x;                      // L1: x
    result[4] += radiance * (x * y);                // L2: xy
    result[5] += radiance * (y * z);                // L2: yz
    result[6] += radiance * (3.0f * z * z - 1.0f);  // L2: 3z^2-1
    result[7] += radiance * (x * z);                // L2: xz
    result[8] += radiance * (x * x - y * y);        // L2: x^2-y^2
  }

  float weight = 1.0f / static_cast<float>(sample_count);
  return ScaleL2(result, weight);
}

// Evaluate SH L2 at a direction (for testing/verification).
inline glm::vec3 EvaluateSHL2(const SHL2& sh, glm::vec3 normal) {
  float x = normal.x, y = normal.y, z = normal.z;
  return sh[0] + sh[1] * y + sh[2] * z + sh[3] * x + sh[4] * x * y +
         sh[5] * y * z + sh[6] * (3.0f * z * z - 1.0f) + sh[7] * x * z +
         sh[8] * (x * x - y * y);
}

// Convert SHL1 to SHL2 (for backwards compatibility)
inline SHL2 SHL1ToSHL2(const SHL1& sh1) {
  SHL2 result{};
  result[0] = sh1[0];
  result[1] = sh1[1];
  result[2] = sh1[2];
  result[3] = sh1[3];
  return result;
}

}  // namespace sh
}  // namespace badlands
