#pragma once

// Task S2.D: MaterialId -> PBR glTF pack mapping. The only game-side piece
// of the material-pack loader -- MaterialLibrary itself (src/engine/
// rendering/material_library.hpp) is engine, keyed purely by (dir, base),
// with no knowledge of MaterialId.

#include "game/material_id.hpp"

namespace badlands {

// A pack's location: `dir` is the pack directory (containing
// `<base>_1k.gltf` + `textures/`), `base` is the glTF/texture filename stem.
// e.g. {"assets/materials/rocky_trail_1k.gltf", "rocky_trail"}.
struct MaterialPack {
  const char* dir;
  const char* base;
};

// Maps each MaterialId to its pack location (see assets/materials/).
MaterialPack material_pack(MaterialId id);

}  // namespace badlands
