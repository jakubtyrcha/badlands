#include "game/material_pack.h"

namespace badlands {

MaterialPack material_pack(MaterialId id) {
  switch (id) {
    case MaterialId::ForestGround:
      return {"assets/materials/forest_ground_04_1k.gltf", "forest_ground_04"};
    case MaterialId::MudLeaves:
      return {"assets/materials/brown_mud_leaves_01_1k.gltf",
              "brown_mud_leaves_01"};
    case MaterialId::RockyTrail:
      return {"assets/materials/rocky_trail_1k.gltf", "rocky_trail"};
    case MaterialId::BrownMud:
      return {"assets/materials/brown_mud_03_1k.gltf", "brown_mud_03"};
    case MaterialId::RiverRocks:
      return {"assets/materials/river_small_rocks_1k.gltf",
              "river_small_rocks"};
    case MaterialId::Bark:
      return {"assets/materials/bark_willow_1k.gltf", "bark_willow"};
    case MaterialId::RockWall:
      return {"assets/materials/rock_wall_16_1k.gltf", "rock_wall_16"};
    case MaterialId::Planks:
      return {"assets/materials/weathered_planks_1k.gltf",
              "weathered_planks"};
    case MaterialId::Plaster:
      return {"assets/materials/plastered_stone_wall_1k.gltf",
              "plastered_stone_wall"};
    case MaterialId::RoofSlates:
      return {"assets/materials/roof_slates_02_1k.gltf", "roof_slates_02"};
    default:
      // Never expected -- MaterialId is a closed 10-value enum with every
      // value handled above; a bad enum value is a programmer error.
      return {"", ""};
  }
}

}  // namespace badlands
