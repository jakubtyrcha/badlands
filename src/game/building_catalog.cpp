#include "game/building_catalog.h"

#include <cstddef>

namespace badlands {

namespace {
constexpr MaterialId kSlateRoof = MaterialId::RoofSlates;

// GameBuildingKind -> display label, in enum declaration order
// (badlands_game.h). Single source of truth for building_label().
struct BuildingLabel {
  GameBuildingKind kind;
  const char* label;
};
constexpr BuildingLabel kBuildingLabels[] = {
    {GAME_BUILDING_CASTLE, "Castle"},
    {GAME_BUILDING_FREE_COMPANY_QUARTERS, "Free Company Quarters"},
    {GAME_BUILDING_HUNTERS_CAMP, "Hunters Camp"},
    {GAME_BUILDING_THIEVES_DEN, "Thieves Den"},
    {GAME_BUILDING_SCRIPTORIUM, "Scriptorium"},
    {GAME_BUILDING_TAVERN, "Tavern"},
    {GAME_BUILDING_APOTHECARY, "Apothecary"},
    {GAME_BUILDING_WATCHTOWER, "Watchtower"},
    {GAME_BUILDING_HOUSE, "House"},
    {GAME_BUILDING_SEWER, "Sewer"},
};
static_assert(sizeof(kBuildingLabels) / sizeof(kBuildingLabels[0]) ==
                  GAME_BUILDING_KIND_COUNT,
              "kBuildingLabels must cover every GameBuildingKind");
}  // namespace

const char* building_label(GameBuildingKind kind) {
  for (const BuildingLabel& b : kBuildingLabels) {
    if (b.kind == kind) return b.label;
  }
  return "?";
}

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
