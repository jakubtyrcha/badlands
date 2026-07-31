#include "mapview/biome_splat.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/biomes.hpp"

namespace badlands {

namespace {

// Separable box blur of one weight plane, radius `r` texels, edges clamped.
// A box (rather than a gaussian) is enough here: the input is one-hot, so any
// low-pass turns the staircase into a ramp, and the top-2 cull afterwards
// discards the tail a wider kernel would have added.
void BlurPlane(std::vector<float>& plane, int w, int h, int r,
               std::vector<float>& scratch) {
  if (r <= 0) return;
  const float inv = 1.0f / static_cast<float>(2 * r + 1);
  scratch.assign(plane.size(), 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k) {
        const int sx = std::clamp(x + k, 0, w - 1);
        sum += plane[static_cast<size_t>(y) * w + sx];
      }
      scratch[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k) {
        const int sy = std::clamp(y + k, 0, h - 1);
        sum += scratch[static_cast<size_t>(sy) * w + x];
      }
      plane[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
}

}  // namespace

BiomeSplat BuildBiomeSplat(const mapgen::Field2D<uint8_t>& biome,
                           float texel_m) {
  BiomeSplat out;
  const int w = biome.width, h = biome.height;
  if (w <= 0 || h <= 0) return out;

  // One plane per biome, one-hot, then blurred.
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<std::vector<float>> planes(mapgen::kBiomeCount,
                                         std::vector<float>(n, 0.0f));
  for (size_t i = 0; i < n; ++i) {
    const int b = std::min<int>(biome.data[i], mapgen::kBiomeCount - 1);
    planes[b][i] = 1.0f;
  }

  const int radius =
      texel_m > 0.0f ? static_cast<int>(std::lround(kBiomeBlendM / texel_m)) : 0;
  std::vector<float> scratch;
  for (auto& p : planes) BlurPlane(p, w, h, radius, scratch);

  out.width = w;
  out.height = h;
  out.slots0.assign(n * 4, 0);
  out.slots1.assign(n * 4, 0);

  for (size_t i = 0; i < n; ++i) {
    // Top 2 by weight. Ties break toward the lower slot, which is stable.
    int best = 0, second = -1;
    for (int b = 1; b < mapgen::kBiomeCount; ++b) {
      if (planes[b][i] > planes[best][i]) best = b;
    }
    for (int b = 0; b < mapgen::kBiomeCount; ++b) {
      if (b == best) continue;
      if (second < 0 || planes[b][i] > planes[second][i]) second = b;
    }
    const float w0 = planes[best][i];
    const float w1 = (second >= 0) ? planes[second][i] : 0.0f;
    const float sum = w0 + w1;
    if (sum <= 0.0f) continue;

    // Quantize so the PAIR sums to exactly 255. A short sum would not darken
    // the blend (the shader renormalizes by the weights it receives), but an
    // exact sum keeps the invariant checkable and the bilinear interpolation
    // between neighbouring texels energy-preserving.
    const int q0 = std::clamp<int>(
        static_cast<int>(std::lround(255.0f * w0 / sum)), 0, 255);

    auto put = [&](int slot, int v) {
      if (v <= 0) return;
      const uint8_t b = static_cast<uint8_t>(v);
      if (slot < 4) {
        out.slots0[i * 4 + slot] = b;
      } else {
        out.slots1[i * 4 + (slot - 4)] = b;
      }
    };
    if (second < 0 || q0 >= 255) {
      // Only one layer here -- give it everything rather than leaving a
      // 254 + 1 split that would sample a second texture for nothing.
      put(best, 255);
    } else {
      put(best, q0);
      put(second, 255 - q0);
    }
  }

  return out;
}

}  // namespace badlands
