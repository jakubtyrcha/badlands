#pragma once

// The lake RECORD -- what every map source, loaded or synthetic, must fill in to
// describe standing water.
//
// Split out of erosion.hpp so that a consumer needing only the vocabulary (the
// patch contract, the river graph's lake nodes) does not drag in a params block
// it has no use for. This header deliberately includes nothing but <cstdint>:
// it is a data type, and anything that makes it heavier to include defeats the
// reason it exists.
//
// The water CONVENTION that goes with it lives on the rasters, not here:
// `depth = max(0, level - height)`, with a dry texel storing `level == height`.
// No sentinel, no mask, and a lake surface is exactly flat by construction --
// `level_m` is the per-lake constant, so no amount of bed detail can tilt it.

#include <cstdint>

namespace badlands::mapgen {

// Where a lake came from. Seeded basins were once placed deliberately by the
// (now-removed) erosion sim; emergent ones are whatever the sim happened to
// pond. Consumers care: a deliberate lake is a map feature, an emergent one is
// incidental.
enum class LakeKind : uint8_t { Seeded, Emergent };

struct LakeInfo {
  LakeKind kind = LakeKind::Emergent;
  float level_m = 0.0f;      // water surface, after the freeboard
  float area_m2 = 0.0f;
  float max_depth_m = 0.0f;
  int32_t outlet_cell = -1;  // the TRUE sill: lowest cell just outside it
};

}  // namespace badlands::mapgen
