#pragma once

// The data contract a map arrives in, whichever of the two sources produced
// it: rasters loaded off disk (mapgen/map_io.hpp, a window simulated by
// tools/protogen/) or the synthetic --test-map forest fixture
// (game/map/forest_test_map_generator.hpp). There is no in-repo generator any
// more -- `MapArtifacts` is a plain struct, not the output of a pure function
// of `MapGenParams` -- but the type stays here under its original name so
// every existing #include of "mapgen/generator.hpp" keeps compiling.
//
// Noise (where a producer samples any) is in world METERS, so the same
// (seed, size_m) at two resolutions is the same map, just sharper. Keep any
// new parameter resolution-independent for the same reason.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/distance_field.hpp"  // distance_to_mask (re-exported, see below)
#include "mapgen/field2d.hpp"
#include "mapgen/lake.hpp"            // LakeInfo, for MapArtifacts::lakes

namespace badlands::mapgen {

struct MapGenParams {
  uint32_t seed = 1;
  int resolution = 512;         // output grid (texels, square)
  float world_size_m = 512.0f;  // world extent (meters, square)
};

// Everything a map source produces.
struct MapArtifacts {
  Field2D<uint8_t> biome;      // Biome enum values, incl. Lake
  Field2D<float> heightmap;    // world meters — ground surface
  Field2D<float> water_depth;  // world meters — standing water; surface = heightmap + water_depth
  // Per-cell lake index into `lakes`, -1 where dry, and the per-lake record.
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  Field2D<float> sediment;     // sediment thickness (m)
};

// distance_to_mask (the exact EDT) moved to mapgen/distance_field.hpp, which
// this header includes — every existing caller keeps compiling against
// "mapgen/generator.hpp" unchanged. It lives there so a consumer can link the
// EDT without dragging in the rest of this header's deps; src/foliage/ is the
// reason.

}  // namespace badlands::mapgen
