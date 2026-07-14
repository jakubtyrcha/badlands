#pragma once

// Terrain/building surface material ids. Mirrors the reference MaterialId's
// 10-layer texture-array order (src/scene/material.rs's PACK_DIRS @ 8ee93cc)
// verbatim — the discriminant is the texture-array layer index the Stage 2.D
// PBR material-pack loader will sample.

namespace badlands {

enum class MaterialId {
  ForestGround = 0,
  MudLeaves,
  RockyTrail,
  BrownMud,
  RiverRocks,
  Bark,
  RockWall,
  Planks,
  Plaster,
  RoofSlates,
};

}  // namespace badlands
