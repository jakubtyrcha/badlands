#include "game/building_catalog.h"

namespace badlands {

namespace {
constexpr MaterialId kSlateRoof = MaterialId::RoofSlates;
}  // namespace

BuildingVisual building_visual(GameBuildingKind kind) {
  switch (kind) {
    case GAME_BUILDING_CASTLE:
      return {.height = 5.0f,
             .roof = RoofShape::CornerTowers,
             .wall_material = MaterialId::RockWall,
             .roof_material = MaterialId::RockWall};
    case GAME_BUILDING_FREE_COMPANY_QUARTERS:
      return {.height = 3.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_HUNTERS_CAMP:
      return {.height = 2.6f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_THIEVES_DEN:
      return {.height = 2.6f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_SCRIPTORIUM:
      return {.height = 3.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_TAVERN:
      return {.height = 2.2f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_APOTHECARY:
      return {.height = 2.2f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_WATCHTOWER:
      return {.height = 4.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::RockWall,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_HOUSE:
      return {.height = 1.8f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case GAME_BUILDING_SEWER:
      return {.height = 0.4f,
             .roof = RoofShape::None,
             .wall_material = MaterialId::RockWall,
             .roof_material = MaterialId::RockWall};
    case GAME_BUILDING_KIND_COUNT:
    default:
      // Never expected (mirrors game_building_def's out-of-range fallback,
      // game/src/placement.cpp's def_of): a 1x1 Watchtower-shaped box.
      return {.height = 4.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::RockWall,
             .roof_material = kSlateRoof};
  }
}

}  // namespace badlands
