#pragma once

// World picking for the game UI: turn a point on the ground plane into the
// building or hero under it.
//
// Ported from the legacy Rust app's src/game_ffi.rs (commit c7cf8d8). The box
// extents come from RenderBoxOf(kind, rotation_index) rather than
// (width_tiles, depth_tiles): diagonal placements snap to a lattice diamond
// whose spans differ from w x d, so the footprint is NOT simply the tile size
// rotated (see RenderBox in badlands_sim.hpp).

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"

namespace badlands {

// No building/hero under the cursor.
inline constexpr uint32_t kNoPick = UINT32_MAX;

// Terrain-aware ground pick. A plain ray/y=0 intersection lands where the cursor
// crosses sea level, but units and buildings render seated on the terrain
// surface -- so on any elevation the y=0 point parallaxes away from a unit's
// small footprint and the click misses where the unit appears. This iterates:
// intersect y=0, sample terrain there, re-intersect at that height, repeat. It
// converges to the terrain-surface XZ under the cursor on gently-varying
// terrain, so a unit standing on that surface is picked where it is drawn.
//
// `height_at` maps a world XZ to the terrain height there (world Y). Returns
// false (leaving out_xz untouched) if the ray does not descend toward the
// ground -- looking up or along the horizon -- matching IntersectGroundPlane.
bool GroundPickXZ(glm::vec3 ray_origin, glm::vec3 ray_dir,
                  const std::function<float(glm::vec2)>& height_at,
                  glm::vec2& out_xz, int iterations = 3);

// The still-valid selected unit, honouring the same "indoors units are not
// selectable" rule HeroAtWorld uses when picking: returns the row whose id is
// `id` only if it exists AND is outside (inside_building_id < 0); nullptr if it
// was destroyed or walked into a building. Callers clear the selection on
// nullptr, so the detail panel never describes a unit that is no longer drawn.
const CharacterState* SelectedUnit(const CharacterState* characters,
                                   uint32_t count, uint32_t id);

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
uint32_t BuildingAtWorld(const BuildingState* buildings, uint32_t count,
                         glm::vec2 world);

// The topmost hero whose footprint contains `world` (x, z), or kNoPick.
// Characters inside a building are skipped -- they are not drawn, so they must
// not be clickable (see CharacterState::inside_building_id in badlands_sim.hpp).
uint32_t HeroAtWorld(const CharacterState* characters, uint32_t count,
                     glm::vec2 world);

}  // namespace badlands
