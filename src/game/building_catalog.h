#pragma once

// Render-only building catalog: display height + roof shape + wall/roof
// materials. Footprint sizes and placement rules live in the C sim
// (game_building_def/game_render_box, badlands_game.h) — this is purely how
// a building looks. Ported from the reference building catalog
// (src/game/catalog.rs @ 8ee93cc's `info(BuildingKind)`), split so the roof
// *shape* (RoofShape, src/core/roof_shape.hpp) is game-agnostic and the
// engine's BuildBuildingParts can consume it directly, while the roof
// *material* stays here as game data.

#include "badlands_game.h"
#include "core/roof_shape.hpp"
#include "game/material_id.hpp"

namespace badlands {

struct BuildingVisual {
  float height;
  RoofShape roof;
  MaterialId wall_material;
  MaterialId roof_material;  // meaningful for Gable/CornerTowers; ignored for None
};

BuildingVisual building_visual(GameBuildingKind kind);

// Human-readable display label for `kind` (e.g. GAME_BUILDING_CASTLE ->
// "Castle"), in enum-declaration order. Returns "?" for an unknown/out-of-
// range value. Shared by the viewer's prefab catalog and the game view's
// building list (each of which used to keep its own copy of this table).
const char* building_label(GameBuildingKind kind);

}  // namespace badlands
