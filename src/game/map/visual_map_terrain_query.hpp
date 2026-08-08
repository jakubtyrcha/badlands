#pragma once

// Binds VisualMapData to the foliage generator's TerrainQuery -- the render-side
// counterpart of map_data_terrain_query.hpp, which does the same for the
// simulation's biome map.
//
// Two of these exist rather than one because the two maps answer different
// questions and the foliage library must learn neither vocabulary. It asks
// three things; this answers them from cover, that one from biomes. A second
// forest type growing in Cover::Shrub is another instance of this class, not a
// change to src/foliage/.

#include "foliage/terrain_query.hpp"
#include "game/map/visual_map_data.hpp"
#include "mapgen/cover.hpp"

namespace badlands {

class VisualMapTerrainQuery final : public foliage::TerrainQuery {
 public:
  // `map` must outlive this query. `cover` is the class whose fraction becomes
  // CoverageAt -- i.e. where this forest type is allowed to grow.
  VisualMapTerrainQuery(const VisualMapData& map, mapgen::Cover cover)
      : map_(&map), cover_(cover) {}

  // Bilinear and edge-clamped, so an off-map query returns the border value
  // rather than failing -- what keeps the generator's edge sampling simple.
  float HeightAt(float x, float z) const override {
    return map_->HeightAt(x, z);
  }

  float CoverageAt(float x, float z) const override {
    return map_->CoverFractionAt(x, z, cover_);
  }

  // Zero, which is what the map view has always reported here: it never set a
  // water level on its map either, so this preserves placement exactly.
  //
  // It is also now WRONG in a way it was not before -- these patches carry real
  // lakes, and a tree may be planted in one. Fixing it belongs to the foliage
  // work, not to a query binding, and inventing a single global level for a
  // patch with several lakes at several elevations would be worse than the
  // omission.
  float WaterLevelM() const override { return 0.0f; }

 private:
  const VisualMapData* map_;
  mapgen::Cover cover_;
};

}  // namespace badlands
