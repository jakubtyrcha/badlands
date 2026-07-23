#pragma once

// Selection highlights: maps a picked unit / building onto the engine's generic
// ProjectedDecal primitive.
//
// Pure geometry -- no rendering, no GPU, no sim access beyond the snapshot rows
// it is handed. That is what makes it unit-testable (see
// game/tests/selection_decals_tests.cpp); GameView just calls these each frame
// for whatever is currently selected and points SceneContext::decals at the
// result.
//
// The look (marching-ants white/black dashes, thickness, margins) is fixed in
// the .cpp. It is not configurable and has no UI: the decal parameters that
// ARE meant to vary live on the engine's ProjectedDecal primitive, and a caller
// wanting a different look builds one directly.

#include "badlands_sim.hpp"  // BuildingState, CharacterState
#include "engine/rendering/projected_decal.hpp"

namespace badlands {

// Rotation index (0..3 -> 0/45/90/135 degrees about +Y) to world yaw. Mirrors
// the PlacementDesc convention in badlands_sim.hpp.
float YawFromRotationIndex(int32_t rotation_index);

// A circle around a unit, sized from its footprint. `ground` is the terrain
// height under the unit -- the decal plane sits there.
ProjectedDecal MakeUnitRing(const CharacterState& unit, float ground);

// A rounded rectangle around a building's DRAWN footprint. Uses
// RenderBoxOf(kind, 0) (the unrotated box) plus the placement yaw, matching how
// the building mesh itself is built -- not the axis-aligned tile footprint,
// which is the box's grid-aligned bounding extent and would sit wrong on a
// rotated building.
ProjectedDecal MakeBuildingRect(const BuildingState& building, float ground);

}  // namespace badlands
