#include "game/map/flat_map_generator.hpp"

namespace badlands {

MapData FlatMapGenerator::Generate() const {
  MapData map(kNodesPerSide, kNodesPerSide, kSpacingM);
  const int plains = static_cast<int>(mapgen::Biome::Plains);
  for (int j = 0; j < kNodesPerSide; ++j) {
    for (int i = 0; i < kNodesPerSide; ++i) {
      // One biome at full weight; every other slice stays at the constructor's
      // zero. A single saturated channel is what makes DominantBiomeAt return
      // Plains everywhere including exactly on the lattice nodes, where a
      // blended pair could otherwise tie.
      map.mutable_slice(plains, i, j) = 255;
      map.mutable_height(i, j) = kHeightM;
    }
  }
  return map;
}

}  // namespace badlands
