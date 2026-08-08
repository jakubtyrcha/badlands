#pragma once

// TERRAIN CLASS: how the ground was carved and what it is made of.
//
// Low-frequency and per-PATCH, not per-texel. It answers "which assets" --
// which rock and ground materials to paint with, which rock props to scatter,
// which tree species -- while the continuous fields (height, soil, water)
// answer "where". Cover (mapgen/cover.hpp) answers "what grows".
//
// These are terrain-net's eight detail classes, the ones the super-resolution
// network conditions on (docs/plan.md in that repo). Elevation-derived classes
// are deliberately absent: hills and mountains are recoverable from the
// heightmap, so a label carrying them carries nothing.
//
// PER PATCH BECAUSE THAT IS WHAT EXISTS. terrain-net emits one label per area in
// manifest.json; a per-texel detail_class.tif is planned. When it ships this
// becomes a raster, and every consumer that reads a scalar today reads a sample
// then -- which is why nothing should cache it as a patch-wide constant.

#include <cstdint>
#include <string_view>

namespace badlands::mapgen {

// Unknown is 0, so an unlabelled patch says so rather than claiming to be the
// first class in the list.
enum class TerrainClass : uint8_t {
  Unknown = 0,
  GlaciatedUpland,  // cirques, aretes, roches moutonnees, hanging valleys
  AlpineTalus,      // scree cones, avalanche chutes, rockfall aprons
  TorBlockfield,    // tors, clitter, gritstone edges, exposed jointing
  ChalkDownland,    // dry valleys, smooth convex-concave, scarp and vale
  KarstPavement,    // clints and grikes, shakeholes, dry gorges
  PeatMoor,         // hags, groughs, gullied blanket bog
  WoodedFluvial,    // V-valleys, ghylls, soil creep, shallow landslip
  LowlandDrift,     // drumlins, kettle holes, roddons, meander scars
};

inline constexpr int kTerrainClassCount = 9;

// MUST match terrain-net's DETAIL_CLASSES strings (fetch/areas.py) -- these are
// parsed straight out of a bundle's manifest.json, so a divergence here reads as
// an unlabelled patch rather than as an error.
inline constexpr std::string_view terrain_class_name(TerrainClass c) {
  switch (c) {
    case TerrainClass::Unknown:
      return "unknown";
    case TerrainClass::GlaciatedUpland:
      return "glaciated_upland";
    case TerrainClass::AlpineTalus:
      return "alpine_talus";
    case TerrainClass::TorBlockfield:
      return "tor_blockfield";
    case TerrainClass::ChalkDownland:
      return "chalk_downland";
    case TerrainClass::KarstPavement:
      return "karst_pavement";
    case TerrainClass::PeatMoor:
      return "peat_moor";
    case TerrainClass::WoodedFluvial:
      return "wooded_fluvial";
    case TerrainClass::LowlandDrift:
      return "lowland_drift";
  }
  return "unknown";
}

// Inverse of terrain_class_name. An unrecognised string is Unknown, never an
// error: a bundle produced by a newer terrain-net with a ninth class should
// still load and render, just without a palette specialisation.
inline constexpr TerrainClass terrain_class_from_name(std::string_view name) {
  for (int i = 0; i < kTerrainClassCount; ++i) {
    const auto c = static_cast<TerrainClass>(i);
    if (terrain_class_name(c) == name) return c;
  }
  return TerrainClass::Unknown;
}

}  // namespace badlands::mapgen
