#include "mapgen/detail_filter.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"  // distance_to_mask
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

constexpr float kSlopeRef = 0.6f;       // slope (m/m) mapping to full gully strength
constexpr float kShoreFadeDistM = 3.0f; // detail fades in over this distance from water
constexpr float kCellPerWavelength = 2.0f;
constexpr float kPersistence = 0.5f;
constexpr float kTwoPi = 6.28318530718f;

// Deterministic 2D integer hash -> [0,1) floats (PCG-style mix).
uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
glm::vec2 hash_pivot(uint32_t seed, int octave, int cx, int cy) {
  const uint32_t h1 = hash_u32(seed * 0x9e3779b9U + octave * 0x85ebca6bU +
                               static_cast<uint32_t>(cx) * 0xc2b2ae35U +
                               static_cast<uint32_t>(cy) * 0x27d4eb2fU);
  const uint32_t h2 = hash_u32(h1 + 0x165667b1U);
  return {static_cast<float>(h1 & 0xffffffU) / 16777216.0f,
          static_cast<float>(h2 & 0xffffffU) / 16777216.0f};
}

// Central-difference gradient of base at a (clamped) texel.
glm::vec2 grad_at(const Field2D<float>& f, int x, int y, float texel_m) {
  const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, f.width - 1);
  const int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, f.height - 1);
  return {(f.at(x1, y) - f.at(x0, y)) / (texel_m * (x1 - x0)),
          (f.at(x, y1) - f.at(x, y0)) / (texel_m * (y1 - y0))};
}

}  // namespace

Field2D<float> gully_detail_delta(const Field2D<float>& base,
                                  const Field2D<float>& water_depth,
                                  float texel_m, uint32_t seed,
                                  const ErosionParams& p) {
  const int n = base.width;
  Field2D<float> delta(n, base.height, 0.0f);
  if (p.detail_octaves <= 0 || p.detail_amplitude_m <= 0.0f) return delta;

  // Distance to standing water, for the shore fade.
  Field2D<uint8_t> wet(n, base.height, 0);
  bool any_wet = false;
  for (size_t i = 0; i < wet.data.size(); ++i) {
    wet.data[i] = water_depth.data[i] > 0.0f ? 1 : 0;
    any_wet |= wet.data[i] != 0;
  }
  const Field2D<float> water_dist =
      any_wet ? distance_to_mask(wet, {texel_m, texel_m}) : Field2D<float>{};

  parallel_tiles(
      n, base.height, 64, [] { return std::monostate{}; },
      [&](std::monostate&, int tx0, int ty0, int tx1, int ty1) {
        for (int y = ty0; y < ty1; ++y) {
          for (int x = tx0; x < tx1; ++x) {
            if (wet.at(x, y)) continue;
            float shore = 1.0f;
            if (any_wet)
              shore = std::clamp(water_dist.at(x, y) / kShoreFadeDistM, 0.0f, 1.0f);
            if (shore <= 0.0f) continue;

            const glm::vec2 g = grad_at(base, x, y, texel_m);
            const float slope = glm::length(g);
            if (slope < 1e-5f) continue;
            const float s01 = std::clamp(slope / kSlopeRef, 0.0f, 1.0f);
            const float slope_mask = 1.0f - (1.0f - s01) * (1.0f - s01);  // ease-out
            if (slope_mask <= 0.0f) continue;
            // across-flow axis: gullies elongate downhill, vary across it
            const glm::vec2 across = glm::normalize(glm::vec2(-g.y, g.x));

            const float wx = static_cast<float>(x) * texel_m;
            const float wy = static_cast<float>(y) * texel_m;
            float carve = 0.0f, ridge_mask = 1.0f;
            float lambda = p.detail_wavelength_m;
            float amp = p.detail_amplitude_m;
            for (int o = 0; o < p.detail_octaves; ++o) {
              const float cell = lambda * kCellPerWavelength;
              const float cxf = wx / cell, cyf = wy / cell;
              const int cx0 = static_cast<int>(std::floor(cxf));
              const int cy0 = static_cast<int>(std::floor(cyf));
              const float fx = cxf - cx0, fy = cyf - cy0;
              // blend the 4 surrounding pivot cells' stripe phases as
              // cos/sin pairs (unit-circle interpolation, then renormalize)
              float bc = 0.0f, bs = 0.0f;
              for (int j = 0; j < 2; ++j) {
                for (int i2 = 0; i2 < 2; ++i2) {
                  const glm::vec2 jit = hash_pivot(seed, o, cx0 + i2, cy0 + j);
                  const glm::vec2 pivot((cx0 + i2 + jit.x) * cell,
                                        (cy0 + j + jit.y) * cell);
                  const float u =
                      glm::dot(glm::vec2(wx, wy) - pivot, across) / lambda;
                  const float wgt = (i2 ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                  bc += wgt * std::cos(kTwoPi * u);
                  bs += wgt * std::sin(kTwoPi * u);
                }
              }
              const float len = std::sqrt(bc * bc + bs * bs);
              const float norm = std::min(1.0f, 2.0f * len);  // blog's k=2 clamp
              const float v = len > 1e-6f ? (bc / len) * norm : 0.0f;  // [-1,1]
              carve += amp * slope_mask * ridge_mask * 0.5f * (v - 1.0f);  // <= 0
              ridge_mask *= std::clamp(1.0f - std::max(0.0f, v), 0.0f, 1.0f);
              lambda *= 0.5f;
              amp *= kPersistence;
            }
            delta.at(x, y) = carve * shore;
          }
        }
      });
  return delta;
}

}  // namespace badlands::mapgen
