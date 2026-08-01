#pragma once

// badlands' forest, loaded as DATA: the bridge between the game-agnostic
// placement library (src/foliage/) and the tree generator (game/geometry/).
//
// The split matters. `foliage::ForestType` carries only what the SAMPLER needs
// -- a footprint radius, a standing height, a depth response -- and its
// `models` are opaque indices. This file is the other half: the same indices,
// paired with the TreeCatalog() preset and variant seed each one actually is.
// Nothing in src/foliage/ can see a TreeOptions, and nothing here changes how
// placement works.
//
// WHY A FILE AND NOT A C++ TABLE. Forest tuning is an iterate-and-look loop:
// change a number, relaunch, judge the screenshot. With the table compiled in,
// every one of those iterations cost a full rebuild -- unaffordable for numbers
// that can only be chosen by eye. The JSON is the single source of truth, with
// no compiled-in defaults shadowing it, so what you read in the file is what
// the forest is.
//
// Presets are referenced BY NAME, not by index into TreeCatalog(): a reordered
// or renamed catalog entry then fails loudly at load instead of silently
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

// The shipped forest definition. Relative to the repo root, like every other
// asset path (apps run from there so `assets/` resolves).
inline constexpr const char* kTemperateForestPath =
    "assets/foliage/temperate_forest.json";

// Loads a forest definition. Returns false (after logging exactly which field
// was wrong) on a missing/unparseable file, an unknown preset name, a malformed
// depth curve, or any out-of-range number; `out` is left empty in that case.
//
// Everything is validated rather than defaulted: a forest that silently ran
// with half its layers because a key was misspelled would be far harder to
// diagnose from a screenshot than a refusal to load.
bool LoadForestCatalog(const std::string& path, ForestCatalog& out);

// Convenience overload for the shipped definition.
inline bool BuildForestCatalog(ForestCatalog& out) {
  return LoadForestCatalog(kTemperateForestPath, out);
}

}  // namespace badlands
