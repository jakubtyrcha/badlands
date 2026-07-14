#pragma once

// Task S2.G: walled AI-sandbox arena. Reuses the game grid's centered-tile
// convention (game/src/placement.h's kGridHalf / in_bounds_tile) so the
// arena's tile<->world mapping matches the sim's placement grid exactly --
// see arena.cpp. Pure data: glm/std only, no engine/SDL/Dawn types
// (badlands_game_lib convention, like game/material_pack.h). AiSandboxView
// (src/ai_sandbox/ai_sandbox_view.cpp) turns this into scene geometry.

#include <vector>

#include <glm/glm.hpp>

namespace badlands {

// Interior (accessible) arena size in tiles, in x/z. Configurable per
// build_arena() call -- e.g. temporarily overridden to {9, 9} to prove the
// arena resizes, then reverted (see task-S2G-report.md).
inline constexpr glm::ivec2 kArenaAccessibleBlocks{13, 7};

struct Arena {
  glm::ivec2 accessible;                 // interior size in tiles
  std::vector<glm::ivec2> floor_tiles;   // accessible interior tiles (grid coords)
  std::vector<glm::ivec2> wall_tiles;    // the 1-tile blocked ring around the interior
};

// Builds a centered arena: `accessible.x` x `accessible.y` interior tiles
// around the origin (same low-biased centering as placement.h's kGridHalf,
// e.g. n=96 -> [-48, 48)) plus a 1-tile blocked wall ring just outside. Every
// tile (interior + ring) is validated in_bounds_tile against the game grid.
Arena build_arena(glm::ivec2 accessible = kArenaAccessibleBlocks);

// World-space XZ center of `tile`, using the game grid's tile=1.0-world-unit
// convention (matches placement.h's triangle_centroid tile-center offset).
glm::vec2 arena_tile_center(glm::ivec2 tile);

}  // namespace badlands
