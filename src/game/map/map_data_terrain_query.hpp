#pragma once

// Binds the frozen MapData contract to the foliage generator's TerrainQuery.
//
// This is the entire game-side half of the foliage library's decoupling: the
// library asks three questions, and this answers them from a MapData. It is
// also where the choice of WHICH biome a forest type grows in is made -- the
// library never learns that `mapgen::Biome` exists, so a second forest type
// growing in Swamp is another instance of this class, not a change to
// src/foliage/.

#include "foliage/terrain_query.hpp"
#include "game/map/map_data.hpp"
#include "mapgen/biomes.hpp"

namespace badlands {

class MapDataTerrainQuery final : public foliage::TerrainQuery {
 public:
  // `map` must outlive this query. `biome` is the slice whose weight becomes
  // CoverageAt -- i.e. where this forest type is allowed to grow.
  MapDataTerrainQuery(const MapData& map, mapgen::Biome biome)
      : map_(&map), slice_(static_cast<int>(biome)) {}

  // Bilinear and edge-clamped, straight off MapData -- an off-map query returns
  // the border value rather than failing, which is what MapData already
  // promises and what keeps the generator's edge sampling simple.
  float HeightAt(float x, float z) const override {
    return map_->HeightAt(x, z);
  }

  float CoverageAt(float x, float z) const override {
    return map_->BiomesAt(x, z).w[slice_];
  }

  float WaterLevelM() const override { return map_->water_level_m(); }

 private:
  const MapData* map_;
  int slice_;
};

}  // namespace badlands
