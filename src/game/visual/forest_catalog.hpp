#pragma once

// badlands' temperate forest, as content: the bridge between the game-agnostic
// placement library (src/foliage/) and the tree generator (game/geometry/).
//
// The split matters. `foliage::ForestType` carries only what the SAMPLER needs
// -- a footprint radius, a standing height, a depth response -- and its
// `models` are opaque indices. This file is the other half: the same indices,
// paired with the TreeCatalog() preset and variant seed each one actually is.
// Nothing in src/foliage/ can see a TreeOptions, and nothing here changes how
// placement works.
//
// Presets are referenced BY NAME, not by index into TreeCatalog(): a reordered
// or renamed catalog entry then fails loudly at build time instead of silently
// planting the wrong species (the convention biome_manifest.hpp already uses
// for the same reason).

#include <string>
#include <vector>

#include "foliage/forest_type.hpp"
#include "game/geometry/tree_options.hpp"

namespace badlands {

// One renderable model: the resolved tree, how tall it stands in the world, and
// a name for diagnostics.
//
// `target_height_m` is a REAL height in metres (an oak at 22 m), not the
// viewer's 8 m preview normalization. It is what the render side scales the
// tree's native ez-tree units to, and it also feeds the per-model voxel-LOD
// scaling -- see BuildTreeField.
struct ForestModelSpec {
  TreeOptions options;
  float target_height_m = 10.0f;
  std::string debug_name;  // e.g. "Oak (large) v2"
};

// A complete forest: the placement data and the models it indexes, parallel and
// the same length (`type.models[i]` describes `models[i]`).
struct ForestCatalog {
  foliage::ForestType type;
  std::vector<ForestModelSpec> models;

  bool empty() const { return models.empty(); }
};

// Builds badlands' forest. Returns false (after logging which preset name it
// could not find) if TreeCatalog() has drifted from the names this file
// expects; `out` is left empty in that case.
//
// Three layers, placed canopy first so it claims its space before the
// undergrowth fills in:
//   canopy   Oak/Pine/Ash/Aspen (large), 4 mesh variants each
//   sapling  Oak/Pine/Ash/Aspen (small), 2 variants each
//   bush     Bush 1 (x2), Bush 2, Bush 3
// 28 models total. Deliberately NOT a species x tier x variant cross-product:
// a tier only exists where a layer uses it, so nothing is generated that
// nothing plants.
bool BuildForestCatalog(ForestCatalog& out);

}  // namespace badlands
