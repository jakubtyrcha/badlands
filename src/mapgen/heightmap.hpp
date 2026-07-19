#pragma once

#include "mapgen/biome_assign.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/config.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/fields.hpp"

namespace badlands::mapgen {

// Compose the final terrain height (world meters) at one sample from the noise
// fields, masked by biome:
//   base    = elevation * height_scale            (smooth rolling base)
//   cavity  = sink low areas below elev_lake       (lake/swamp basins, smooth)
//   detail  = ridged * hills_ridge in Hills only   (organic ridges)
//   variation = small per-biome noise so no region is dead-flat
// Pure per-pixel function — exposed for unit testing.
float compose_height(const MapgenConfig& cfg, Biome biome, float elevation,
                     float ridged, float fine);

// Apply compose_height over the whole map (parallel), producing the heightmap
// in world meters.
Field2D<float> compose_heightmap(const MapgenConfig& cfg, const Fields& fields,
                                 const BiomeMap& biomes);

}  // namespace badlands::mapgen
