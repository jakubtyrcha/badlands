#include "game/arena.h"

#include <spdlog/spdlog.h>

#include "placement.h"  // kGridHalf, in_bounds_tile -- the game grid reuse point

namespace badlands {

namespace {

// Low-biased centering matching placement.h's kGridHalf convention: for an
// axis of length n, tiles span [lo, lo+n) with lo = -(n/2) (integer
// division) -- e.g. kGridHalf's n=96 gives [-48, 48); an odd n=13 gives
// [-6, 7) i.e. tiles -6..6 inclusive.
int LowBound(int n) { return -(n / 2); }

}  // namespace

Arena build_arena(glm::ivec2 accessible) {
  Arena arena;
  arena.accessible = accessible;

  const int lo_x = LowBound(accessible.x);
  const int hi_x = lo_x + accessible.x - 1;  // inclusive
  const int lo_z = LowBound(accessible.y);
  const int hi_z = lo_z + accessible.y - 1;  // inclusive

  // The wall ring is the 1-tile border just outside the interior box.
  const int wall_lo_x = lo_x - 1;
  const int wall_hi_x = hi_x + 1;
  const int wall_lo_z = lo_z - 1;
  const int wall_hi_z = hi_z + 1;

  if (!in_bounds_tile(wall_lo_x, wall_lo_z) || !in_bounds_tile(wall_hi_x, wall_hi_z)) {
    spdlog::error(
        "build_arena: accessible {}x{} wall ring exceeds the game grid "
        "bounds (kGridHalf={}) -- returning an empty arena",
        accessible.x, accessible.y, kGridHalf);
    return arena;
  }

  arena.floor_tiles.reserve(static_cast<size_t>(accessible.x) *
                            static_cast<size_t>(accessible.y));
  for (int tz = lo_z; tz <= hi_z; ++tz) {
    for (int tx = lo_x; tx <= hi_x; ++tx) {
      arena.floor_tiles.push_back(glm::ivec2(tx, tz));
    }
  }

  for (int tz = wall_lo_z; tz <= wall_hi_z; ++tz) {
    for (int tx = wall_lo_x; tx <= wall_hi_x; ++tx) {
      const bool interior = tx >= lo_x && tx <= hi_x && tz >= lo_z && tz <= hi_z;
      if (!interior) {
        arena.wall_tiles.push_back(glm::ivec2(tx, tz));
      }
    }
  }

  return arena;
}

glm::vec2 arena_tile_center(glm::ivec2 tile) {
  return {static_cast<float>(tile.x) + 0.5f, static_cast<float>(tile.y) + 0.5f};
}

}  // namespace badlands
