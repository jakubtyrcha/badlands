#pragma once

// Pure, GPU-free RGBA8 checkerboard fill — the unit-testable core of
// MaterialLibrary::CheckerAlbedo's debug material. Header-only (mirrors
// engine/rendering/decal_math.hpp). Engine, game-agnostic.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands {

// Builds a `texels`x`texels` RGBA8 checkerboard (row-major, tightly packed,
// alpha=255). `tiles` tiles per side; tile (ti,tj) is `color_a` when (ti+tj)
// is even, else `color_b`. Colors are raw sRGB (quantized to 8-bit), matching
// SolidColor's convention. `tiles>=1`, `texels>=1` assumed.
inline std::vector<uint8_t> BuildCheckerboardRgba8(glm::vec3 color_a,
                                                   glm::vec3 color_b, int tiles,
                                                   int texels) {
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t a[3] = {to_byte(color_a.r), to_byte(color_a.g), to_byte(color_a.b)};
  const uint8_t b[3] = {to_byte(color_b.r), to_byte(color_b.g), to_byte(color_b.b)};
  const int tile_px = std::max(1, texels / std::max(1, tiles));

  std::vector<uint8_t> out(static_cast<size_t>(texels) * texels * 4);
  for (int y = 0; y < texels; ++y) {
    const int tj = std::min(y / tile_px, tiles - 1);
    for (int x = 0; x < texels; ++x) {
      const int ti = std::min(x / tile_px, tiles - 1);
      const uint8_t* c = ((ti + tj) & 1) ? b : a;
      const size_t o = (static_cast<size_t>(y) * texels + x) * 4;
      out[o + 0] = c[0];
      out[o + 1] = c[1];
      out[o + 2] = c[2];
      out[o + 3] = 255;
    }
  }
  return out;
}

}  // namespace badlands
