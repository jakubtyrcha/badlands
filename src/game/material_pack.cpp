#include "game/material_pack.h"

namespace badlands {

MaterialPack material_pack(MaterialId id) {
  switch (id) {
    case MaterialId::ForestGround:
      return {"assets/materials/forest_ground_06_1k"};
    case MaterialId::MudLeaves:
      return {"assets/materials/brown_mud_leaves_01_1k"};
    case MaterialId::RockyTrail:
      return {"assets/materials/rocky_terrain_02_1k"};
    case MaterialId::BrownMud:
      return {"assets/materials/brown_mud_03_1k"};
    case MaterialId::RiverRocks:
      return {"assets/materials/river_small_rocks_1k"};
    case MaterialId::Bark:
      return {"assets/materials/bark_willow_1k"};
    case MaterialId::RockWall:
      return {"assets/materials/red_bricks_02_1k"};
    case MaterialId::Planks:
      return {"assets/materials/weathered_planks_1k"};
    case MaterialId::Plaster:
      return {"assets/materials/red_bricks_02_1k"};
    case MaterialId::RoofSlates:
      return {"assets/materials/roof_slates_02_1k"};
    default:
      // Never expected -- MaterialId is a closed 10-value enum with every
      // value handled above; a bad enum value is a programmer error.
      return {""};
  }
}

}  // namespace badlands
