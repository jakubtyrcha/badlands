#include "mapgen/smooth.hpp"

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

Field2D<float> smooth_heightmap(const Field2D<float>& h, float texel_m,
                                float sigma_m, float strength) {
  if (h.width <= 0 || h.height <= 0) return h;
  if (!(sigma_m > 0.0f) || !(strength > 0.0f) || !(texel_m > 0.0f)) return h;

  // Kernel in TEXELS, derived from a world-metre sigma, so the same parameters
  // blur the same physical distance at any resolution.
  const float sigma_t = sigma_m / texel_m;
  const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma_t)));
  std::vector<float> kernel(static_cast<size_t>(2 * radius + 1));
  const float inv2s2 = 1.0f / (2.0f * sigma_t * sigma_t);
  float sum = 0.0f;
  for (int k = -radius; k <= radius; ++k) {
    const float v = std::exp(-static_cast<float>(k * k) * inv2s2);
    kernel[static_cast<size_t>(k + radius)] = v;
    sum += v;
  }
  // Normalized to 1, so a constant field is preserved exactly.
  for (float& v : kernel) v /= sum;

  const int w = h.width, ht = h.height;
  Field2D<float> tmp(w, ht, 0.0f), out(w, ht, 0.0f);

  // Horizontal pass, then vertical. Edges clamp-extend.
  parallel_tiles(w, ht, 64, [] { return std::monostate{}; },
                 [&](std::monostate&, int x0, int y0, int x1, int y1) {
                   for (int y = y0; y < y1; ++y)
                     for (int x = x0; x < x1; ++x) {
                       float acc = 0.0f;
                       for (int k = -radius; k <= radius; ++k)
                         acc += kernel[static_cast<size_t>(k + radius)] *
                                h.at(std::clamp(x + k, 0, w - 1), y);
                       tmp.at(x, y) = acc;
                     }
                 });
  parallel_tiles(w, ht, 64, [] { return std::monostate{}; },
                 [&](std::monostate&, int x0, int y0, int x1, int y1) {
                   for (int y = y0; y < y1; ++y)
                     for (int x = x0; x < x1; ++x) {
                       float acc = 0.0f;
                       for (int k = -radius; k <= radius; ++k)
                         acc += kernel[static_cast<size_t>(k + radius)] *
                                tmp.at(x, std::clamp(y + k, 0, ht - 1));
                       const float s = std::clamp(strength, 0.0f, 1.0f);
                       out.at(x, y) = h.at(x, y) + (acc - h.at(x, y)) * s;
                     }
                 });
  return out;
}

}  // namespace badlands::mapgen
