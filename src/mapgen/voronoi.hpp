#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "mapgen/config.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Voronoi pre-sections: a jittered-grid seed set and the per-pixel nearest-seed
// cell id. Each cell becomes one biome (assigned by sampling the elevation +
// moisture fields at its seed), so cells are the "pre-sections" of the design.
struct Voronoi {
  int cols = 0;
  int rows = 0;
  std::vector<glm::vec2> seeds;  // indexed by cell id (= gy*cols + gx), in
                                 // pixel coordinates
  Field2D<int> cell;             // per-pixel nearest-seed cell id

  int cell_count() const { return cols * rows; }
};

// Build the voronoi decomposition deterministically from cfg.seed.
Voronoi build_voronoi(const MapgenConfig& cfg);

}  // namespace badlands::mapgen
