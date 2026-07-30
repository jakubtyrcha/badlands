#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
namespace badlands {
// Rasterize a leaf silhouette into a tightly-packed size*size RGBA8 buffer (row-major).
// RGB = leaf_color (constant, present even in transparent regions to avoid mip halos);
// alpha = leaf shape (255 inside, 0 outside, ~2-texel soft edge). Deterministic, no RNG.
std::vector<uint8_t> BuildLeafRgba8(int size, glm::vec3 leaf_color);
}  // namespace badlands
