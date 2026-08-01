#pragma once

// The foliage generator's ONLY view of the world it plants into.
//
// This interface is the whole decoupling story. `src/foliage/` is a game world
// component, not a mapview feature and not a game feature: it must run against
// a procedural map, an authored map, a synthetic test map, or a unit-test stub
// with no code change. So it does not take MapData, does not take a Field2D,
// and does not know what a biome is.
//
// `CoverageAt` is deliberately PER-FOREST-TYPE rather than "give me the biome
// here": the caller decides which biome (or blend of biomes, or authored mask)
// a given forest grows in and binds it to this one scalar. That is what keeps
// mapgen::Biome out of this library entirely -- the game-side adapter binds
// Biome::Forest, a test binds a disc, and neither is visible from in here.
//
// Slope is NOT a query. It is derived inside the library by central
// differences on HeightAt (see scatter.cpp), so an implementer only has to
// answer three questions, and every implementation gets the same slope
// definition rather than each inventing one.

namespace badlands::foliage {

struct TerrainQuery {
  virtual ~TerrainQuery() = default;

  // Ground height at world XZ, in metres. Must be defined everywhere the
  // generator is asked to cover (implementations typically clamp off-map
  // queries to the border rather than failing -- MapData::HeightAt does).
  virtual float HeightAt(float x, float z) const = 0;

  // How strongly THIS forest type's biome covers world XZ, in [0, 1].
  // 0 means "this forest does not grow here" and is the generator's first and
  // cheapest rejection test.
  virtual float CoverageAt(float x, float z) const = 0;

  // World Y of the water surface. Nothing is planted below it (plus a small
  // clearance -- see kWaterClearanceM in scatter.hpp).
  virtual float WaterLevelM() const = 0;
};

}  // namespace badlands::foliage
