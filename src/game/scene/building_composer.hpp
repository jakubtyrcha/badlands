#pragma once

// Adds a building's blocky parts to the visual SceneComposer instead of directly
// to the SceneGraph (cf. building_scene.h's AddBuildingToScene, used by the model
// viewer). Same geometry/placement; the composer's RenderMode picks the material
// (blockout: wall/roof debug colors; detailed: the part's PBR pack). Game-layer.

#include <glm/glm.hpp>

#include "badlands_game.h"  // GameBuildingKind
#include "game/visual/scene_composer.hpp"

namespace badlands {

// `ground_y` lifts the building so its base sits on the terrain surface at
// `center_world` (the parts are built with their base at local y=0).
void AddBuildingToComposer(SceneComposer& composer, GameBuildingKind kind,
                           glm::vec2 center_world, float yaw_radians,
                           float ground_y = 0.0f);

}  // namespace badlands
