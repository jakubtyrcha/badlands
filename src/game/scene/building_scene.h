#pragma once

// Task S2.F: shared building-assembly helper, factored out of
// ModelViewerView::AddPrefab's building branch (src/viewer/
// model_viewer_view.cpp) so badlands_game's GameView (and, potentially,
// ModelViewerView) don't re-derive the same Wall->wall_material,
// Roof/Tower->roof_material assembly. Game-layer, not engine (CLAUDE.md's
// layer boundary): takes GameBuildingKind and reads game::building_visual /
// game::material_pack.

#include <glm/glm.hpp>

#include "badlands_game.h"  // GameBuildingKind
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

// Adds one building's parts (BuildBuildingParts, dimensioned from
// game_render_box(kind, /*rotation_index=*/0)'s LOCAL, pre-rotation
// size_x/size_z + building_visual(kind)'s height/roof; Wall parts get
// wall_material, Roof/Tower parts get roof_material, both resolved via
// `matlib`) to `scene`, positioned at
// translate(center_world.x, 0, center_world.y) * rotateY(yaw_radians).
//
// `yaw_radians` is the building's actual world orientation, supplied by the
// caller (e.g. badlands_game.h's documented rotation_index*45deg convention)
// -- kept separate from the rotation_index passed to game_render_box (always
// 0 here) because game_render_box's diagonal (odd rotation_index) branch
// returns footprint dims already swapped for a 45deg-diamond lattice, which
// this helper does not reproduce. Applying a raw idx*45deg yaw to the
// orthogonal box is exact for rotation_index 0/2 and an approximation for
// 1/3 -- fine for this stage, whose buildings are all placed orthogonally.
void AddBuildingToScene(SceneGraph& scene, MaterialLibrary& matlib,
                        GameBuildingKind kind, glm::vec2 center_world,
                        float yaw_radians);

}  // namespace badlands
