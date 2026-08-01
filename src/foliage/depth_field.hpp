#pragma once

// Depth-into-forest, in world METRES: the field that drives every "young at the
// edge, old inside" decision.
//
// WHY NOT THE BIOME BLEND. The obvious source for an edge gradient is the biome
// coverage weight itself, and it is the wrong one. A forest edge is a light and
// microclimate gradient with a real physical length scale -- edge effects
// penetrate roughly 10-30 m in temperate forest -- whereas a coverage weight is
// a ratio whose transition width is an artifact of raster resolution and blur
// radius. On badlands' own maps, MapData's slices are one-hot per texel, so the
// blend interpolates across a SINGLE texel (~1 m); mapview's splat adds a 3 m
// blur. Driving edge structure off that gives a 1-3 m ring that vanishes the
// moment the map resolution changes. An exact EDT gives a distance in metres
// that means the same thing at any resolution.
//
// WHY IT IS WARPED. A raw EDT threshold produces a boundary that is a perfect
// offset curve of the biome mask -- geometrically correct, and unmistakably
// artificial. Perturbing depth by an fBm before it is read makes the tree line
// ragged, which is what actually reads as a forest edge.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "foliage/forest_type.hpp"  // ForestNoise
#include "foliage/terrain_query.hpp"

namespace badlands::foliage {

// Coverage at or above this counts as "inside the forest" for the purpose of
// building the mask the EDT measures from. A threshold, not a blend: the depth
// field's whole job is to replace the blend with a distance.
inline constexpr float kCoverageMaskThreshold = 0.5f;

// Depth assigned when the sampled rect is ENTIRELY inside the forest, i.e.
// there is no edge on the map to measure a distance to. Large enough that every
// plausible DepthCurve is at its plateau, so a fully forested map grows mature
// interior forest instead of the nothing an all-zero EDT would give.
inline constexpr float kInteriorDepthM = 1.0e4f;

// A sampled raster of depth-into-forest. Values are >= 0; 0 means "outside the
// forest, or right on its edge".
struct DepthField {
  glm::vec2 origin_m{0.0f};  // world XZ of texel (0,0)
  float texel_m = 1.0f;
  int width = 0;
  int height = 0;
  std::vector<float> depth;  // metres, row-major

  bool empty() const { return width <= 0 || height <= 0; }

  // Bilinear sample, clamped to the raster edge (an off-raster query returns
  // the border value rather than failing -- mirrors MapData::HeightAt).
  float DepthAt(float x, float z) const;
};

// Builds the depth field over [origin_m, origin_m + size_m] at `texel_m`
// spacing, by thresholding `query.CoverageAt` into a mask, running the exact
// EDT (mapgen::distance_to_mask) on its complement, and warping the result with
// `noise`.
//
// `seed` offsets the warp noise so two forests on one map do not share an edge
// pattern. Passing a `noise` with warp_amp_m <= 0 skips the warp entirely,
// which is what the tests use to check the depth against an analytic answer.
DepthField BuildDepthField(const TerrainQuery& query, glm::vec2 origin_m,
                           glm::vec2 size_m, float texel_m,
                           const ForestNoise& noise, uint32_t seed);

}  // namespace badlands::foliage
