#pragma once

// The plopper: a ForestType + a TerrainQuery in, a cell-bucketed FoliageField
// out.
//
// THE SAMPLER is a jittered grid with multi-class rejection. Per layer, exactly
// one candidate is tried per grid_m square, its position and every subsequent
// roll coming from hash(seed, layer, gx, gz). A candidate survives only if it
// clears, in this order: coverage, a density roll, terrain (slope + water), and
// spacing against everything already placed.
//
// Why this and not Bridson Poisson-disk: Bridson needs a global active list,
// which makes it order-dependent and awkward to parallelize, and it gives one
// radius per run. Here the exclusion radius is per MODEL -- each model's
// MEASURED crown radius -- and the rule between two instances is
// r_i * s_i + r_j * s_j, a sum, so no two crown circles overlap and a bush
// stands clear of an oak's drip line rather than merely of its trunk. Bushes
// still pack tightly among themselves, because their own radii are small. That
// is multi-class blue noise, without the global state.
//
// Layers are placed in declaration order and earlier instances block later
// ones, so ordering the canopy first is what lets it claim its space before the
// undergrowth fills the gaps.
//
// DETERMINISM is a hard guarantee: the loops are strictly ordered and
// single-threaded, and all randomness is integer hashing (foliage/hash.hpp), so
// the same seed gives a byte-identical field. Nothing here is hot -- a 512 m map
// at 6 m canopy spacing is ~7k candidates.

#include <cstdint>

#include <glm/glm.hpp>

#include "foliage/foliage_types.hpp"
#include "foliage/forest_type.hpp"
#include "foliage/terrain_query.hpp"

namespace badlands::foliage {

// Nothing is planted within this many metres (vertically) of the water surface.
// A margin rather than a strict test, so trees do not stand in the shallows.
inline constexpr float kWaterClearanceM = 0.3f;

// Instances are sunk this far into the ground. Trunks generated with their base
// at local y=0 otherwise appear to float on any slope, since the ground falls
// away under the far side of the trunk.
inline constexpr float kGroundSinkM = 0.15f;

// Half-step for the central-difference slope probe on TerrainQuery::HeightAt.
// Half a metre reads the local slope a tree actually sits on, without picking
// up per-texel heightmap noise the way a much smaller step would.
inline constexpr float kSlopeProbeM = 0.5f;

struct FoliageGenParams {
  uint32_t seed = 1;
  glm::vec2 origin_m{0.0f};  // world XZ of the region's minimum corner
  glm::vec2 size_m{128.0f};  // region extent, metres
  // Raster spacing for the coverage mask the depth field is built from. 1 m
  // resolves an edge finely enough that the warp, not the raster, decides how
  // ragged the tree line is.
  float mask_texel_m = 1.0f;
};

// Places `forest` over the requested region. Returns an empty field (after
// logging) if the ForestType is malformed (ForestType::Valid) or the region is
// degenerate; never throws, never fails partially.
FoliageField GenerateFoliage(const ForestType& forest,
                             const TerrainQuery& query,
                             const FoliageGenParams& params);

// True if ANY point of the region's coverage raster is non-zero -- i.e. whether
// this forest type has anywhere at all to grow.
//
// Exists because a consumer whose models are expensive to build cannot learn
// this from an empty FoliageField: placement spaces by each model's MEASURED
// crown, so the models must exist before GenerateFoliage runs, and a field that
// comes back empty has already paid for them. On a map with no forest biome
// that was ~1 s of mesh generation discarded on every load.
//
// Samples the same raster BuildDepthField would, so it sees exactly what
// placement will: a patch too small to register here is one that would have
// been thresholded away regardless.
bool AnyCoverage(const TerrainQuery& query, const FoliageGenParams& params);

// Slope in DEGREES at world XZ, by central differences on query.HeightAt.
// Exposed because it is a distinct method with a checkable answer -- the tests
// drive it against an analytic ramp.
float SlopeDegreesAt(const TerrainQuery& query, float x, float z);

// The clump fBm remapped through [lo, hi] and clamped to [0, 1]. Exposed for
// the same reason: the window is what opens glades rather than merely thinning
// the forest, and it is worth pinning independently of a whole generation run.
float RemapClump(float raw_noise, float lo, float hi);

}  // namespace badlands::foliage
