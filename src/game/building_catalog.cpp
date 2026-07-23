#include "game/building_catalog.h"

#include <cstddef>

namespace badlands {

namespace {
constexpr MaterialId kSlateRoof = MaterialId::RoofSlates;

// BuildingKind -> display label, in enum declaration order
// (badlands_sim.hpp). Single source of truth for building_label().
struct BuildingLabel {
  BuildingKind kind;
  const char* label;
};
constexpr BuildingLabel kBuildingLabels[] = {
    {BuildingKind::Castle, "Castle"},
    {BuildingKind::FreeCompanyQuarters, "Free Company Quarters"},
    {BuildingKind::HuntersCamp, "Hunters Camp"},
    {BuildingKind::ThievesDen, "Thieves Den"},
    {BuildingKind::Scriptorium, "Scriptorium"},
    {BuildingKind::Tavern, "Tavern"},
    {BuildingKind::Apothecary, "Apothecary"},
    {BuildingKind::Watchtower, "Watchtower"},
    {BuildingKind::House, "House"},
    {BuildingKind::Sewer, "Sewer"},
};
static_assert(sizeof(kBuildingLabels) / sizeof(kBuildingLabels[0]) ==
                  static_cast<size_t>(BuildingKind::Count),
              "kBuildingLabels must cover every BuildingKind");
}  // namespace

const char* building_label(BuildingKind kind) {
  for (const BuildingLabel& b : kBuildingLabels) {
    if (b.kind == kind) return b.label;
  }
  return "?";
}

BuildingVisual building_visual(BuildingKind kind) {
  switch (kind) {
    case BuildingKind::Castle:
      return {.height = 5.0f,
             .roof = RoofShape::CornerTowers,
             .wall_material = MaterialId::RockWall,
             .roof_material = MaterialId::RockWall};
    case BuildingKind::FreeCompanyQuarters:
      return {.height = 3.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case BuildingKind::HuntersCamp:
      return {.height = 2.6f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case BuildingKind::ThievesDen:
      return {.height = 2.6f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case BuildingKind::Scriptorium:
      return {.height = 3.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case BuildingKind::Tavern:
      return {.height = 2.2f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Planks,
             .roof_material = kSlateRoof};
    case BuildingKind::Apothecary:
      return {.height = 2.2f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case BuildingKind::Watchtower:
      return {.height = 4.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::RockWall,
             .roof_material = kSlateRoof};
    case BuildingKind::House:
      return {.height = 1.8f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::Plaster,
             .roof_material = kSlateRoof};
    case BuildingKind::Sewer:
      return {.height = 0.4f,
             .roof = RoofShape::None,
             .wall_material = MaterialId::RockWall,
             .roof_material = MaterialId::RockWall};
    case BuildingKind::Count:
    default:
      // Never expected (mirrors BuildingDefOf's out-of-range fallback,
      // game/src/placement.cpp's def_of): a 1x1 Watchtower-shaped box.
      return {.height = 4.0f,
             .roof = RoofShape::Gable,
             .wall_material = MaterialId::RockWall,
             .roof_material = kSlateRoof};
  }
}

}  // namespace badlands
