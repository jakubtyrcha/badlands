#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "game/geometry/tree_options.hpp"
namespace badlands {
// Rasterize a leaf silhouette into a tightly-packed size*size RGBA8 buffer (row-major).
// RGB = leaf_color (constant, present even in transparent regions to avoid mip halos);
// alpha = leaf shape (255 inside, 0 outside, ~2-texel soft edge). Deterministic, no RNG.
std::vector<uint8_t> BuildLeafRgba8(int size, glm::vec3 leaf_color, LeafSilhouette shape);

// Full RGBA8 mip chain (level 0 = size*size down to 1*1), box-filtered. Each
// level's alpha is rescaled so its coverage at `alpha_cutoff` (fraction of
// texels with alpha >= cutoff*255) matches level 0's coverage -- Castano
// alpha-coverage preservation, so alpha-cutout foliage doesn't fade out at
// distance. RGB stays the flat leaf_color at every level.
std::vector<std::vector<uint8_t>> BuildLeafMipChainRgba8(int size, glm::vec3 leaf_color,
                                                           LeafSilhouette shape,
                                                           float alpha_cutoff);
}  // namespace badlands
