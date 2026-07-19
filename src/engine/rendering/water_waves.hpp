#pragma once

// Water surface wave math — the CPU source-of-truth for the water material's
// vertex height displacement and its analytic surface normal. The water is a
// pure XZ height field (height only, no XZ displacement), so the normal is the
// standard height-field normal normalize(vec3(-dH/dx, 1, -dH/dz)).
//
// shaders/common/water_waves.wesl MUST mirror kWaveCount, DefaultWaves() and
// these formulas exactly; badlands_water_gpu_test reads back the shader-written
// normal from the G-buffer and asserts it matches WaveNormal() here.
//
// Header-only, glm-only (no Dawn) — usable from pure-CPU test targets.

#include <array>
#include <cmath>

#include <glm/glm.hpp>

namespace badlands::water {

// A single directional sine wave on the XZ height field.
//   height contribution = amplitude * sin(dot(dir, xz) * frequency
//                                          + t * speed + phase)
struct Wave {
  glm::vec2 dir{1.0f, 0.0f};  // unit direction in XZ
  float amplitude{0.0f};      // metres
  float frequency{0.0f};      // radians per metre (spatial)
  float speed{0.0f};          // radians per second (temporal)
  float phase{0.0f};          // radians
};

inline constexpr int kWaveCount = 3;

using WaveSet = std::array<Wave, kWaveCount>;

// Canonical low-frequency wave set. MUST match shaders/common/water_waves.wesl.
inline const WaveSet& DefaultWaves() {
  static const WaveSet waves = {{
      {glm::normalize(glm::vec2(1.0f, 0.0f)), 0.18f, 0.35f, 0.9f, 0.0f},
      {glm::normalize(glm::vec2(0.6f, 0.8f)), 0.12f, 0.60f, 1.1f, 1.7f},
      {glm::normalize(glm::vec2(-0.8f, 0.4f)), 0.07f, 1.10f, 1.6f, 4.2f},
  }};
  return waves;
}

// Surface height above the rest plane at world XZ `xz` and time `t` (seconds).
inline float WaveHeight(glm::vec2 xz, float t, const WaveSet& waves) {
  float h = 0.0f;
  for (const auto& w : waves) {
    float p = glm::dot(w.dir, xz) * w.frequency + t * w.speed + w.phase;
    h += w.amplitude * std::sin(p);
  }
  return h;
}

// Analytic (unit) height-field normal of WaveHeight at `xz`, `t`.
inline glm::vec3 WaveNormal(glm::vec2 xz, float t, const WaveSet& waves) {
  float dhdx = 0.0f;
  float dhdz = 0.0f;
  for (const auto& w : waves) {
    float p = glm::dot(w.dir, xz) * w.frequency + t * w.speed + w.phase;
    float c = w.amplitude * std::cos(p) * w.frequency;
    dhdx += c * w.dir.x;
    dhdz += c * w.dir.y;
  }
  return glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
}

}  // namespace badlands::water
