#pragma once

#include <cstdint>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/config.hpp"
#include "mapgen/fields.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/voronoi.hpp"

namespace badlands::mapgen {

// Biome assignment result: one biome per voronoi cell (Whittaker-style from the
// elevation + moisture fields sampled at each cell's seed), plus the expanded
// per-pixel biome map. Adjacent same-biome cells form the coherent biome
// regions ("clusters") emergently.
struct BiomeMap {
  std::vector<Biome> cell_biome;  // indexed by voronoi cell id
  Field2D<uint8_t> pixel;         // per-pixel biome (value is a Biome)
};

BiomeMap assign_biomes(const MapgenConfig& cfg, const Voronoi& voronoi,
                       const Fields& fields);

// Classify a single (elevation, moisture) pair in [0,1] into a biome. Exposed
// for unit testing the threshold logic.
Biome classify_biome(float elevation, float moisture, const MapgenConfig& cfg);

}  // namespace badlands::mapgen
