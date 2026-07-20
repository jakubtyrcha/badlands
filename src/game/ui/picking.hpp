#pragma once

// World picking for the game UI: turn a point on the ground plane into the
// building or hero under it.
//
// Ported from the legacy Rust app's src/game_ffi.rs (commit c7cf8d8). The box
// extents come from game_render_box(kind, rotation_index) rather than
// (width_tiles, depth_tiles): diagonal placements snap to a lattice diamond
// whose spans differ from w x d, so the footprint is NOT simply the tile size
// rotated (badlands_game.h:112-121).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "badlands_game.h"

namespace badlands {

// No building/hero under the cursor.
inline constexpr uint32_t kNoPick = UINT32_MAX;

// Point-in-oriented-box in the ground (XZ) plane. `p` and `center` are (x, z);
// `half` is the box's half-extents; `yaw` is the same angle the renderer feeds
// to glm::rotate about +Y.
//
// SIGN WARNING: the 2D "y" component here is world Z. A rotation about +Y maps
// (1,0,0) -> (0,0,-1), i.e. (1,0) -> (0,-1) in (x,z) -- a NEGATIVE-angle 2D
// rotation. So the inverse rotation used to test in the box's local frame is a
// POSITIVE 2D rotation, which is why the sine term below is +s in the second
// row and not the "obvious" -s. Getting this backwards still looks correct for
// every axis-aligned building and silently mis-picks diagonal ones; the
// handedness is pinned by a test.
bool PointInOrientedBox(glm::vec2 center, glm::vec2 half, float yaw,
                        glm::vec2 p);

// The topmost building whose footprint contains `world` (x, z), or kNoPick.
// Later (last-placed) buildings win an overlap, matching draw order.
uint32_t BuildingAtWorld(const GameBuildingState* buildings, uint32_t count,
                         glm::vec2 world);

// The topmost hero whose footprint contains `world` (x, z), or kNoPick.
// Characters inside a building are skipped -- they are not drawn, so they must
// not be clickable (badlands_game.h:41).
uint32_t HeroAtWorld(const GameCharacterState* characters, uint32_t count,
                     glm::vec2 world);

}  // namespace badlands
