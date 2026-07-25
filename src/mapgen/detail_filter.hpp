#pragma once

#include <cstdint>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Analytic slope-aligned gully octaves (runevision-style, v1 core subset:
// pivot-cell oriented stripes, cos/sin renormalization, stacked ridge fading,
// ease-out slope mask). Returns the CARVE delta (<= 0) to add to `base`.
// Zero where water stands; fades in over kShoreFadeHeightM of height above
// the nearest water surface (found by a bounded BFS); zero on flats.
// Pure per-texel function of (world pos, seed, base gradient, nearby water):
// deterministic.
Field2D<float> gully_detail_delta(const Field2D<float>& base,
                                  const Field2D<float>& water_depth,
                                  float texel_m, uint32_t seed,
                                  const ErosionParams& p);

}  // namespace badlands::mapgen
