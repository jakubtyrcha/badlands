#pragma once

// Task S2.D (M3: repointed to the JSON-manifest packs): MaterialId -> PBR
// pack mapping. The only game-side piece of the material-pack loader --
// MaterialLibrary itself (src/engine/rendering/material_library.hpp) is
// engine, keyed purely by pack directory, with no knowledge of MaterialId.

#include "game/material_id.hpp"

namespace badlands {

// A pack's location: `dir` is the pack directory (containing
// `material.json` + `textures/`). e.g.
// {"assets/materials/rocky_terrain_02_1k"}.
struct MaterialPack {
  const char* dir;
};

// Maps each MaterialId to its pack location (see assets/materials/).
MaterialPack material_pack(MaterialId id);

}  // namespace badlands
