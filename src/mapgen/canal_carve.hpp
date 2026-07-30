#pragma once

// Agent-based canal pre-carve: scratches a cheap drainage skeleton into the
// plains BEFORE the erosion sim runs, so the hydrology has something to follow
// instead of an epsilon-flooded flat.
//
// This is a PRIMER, not a simulation. It moves no sediment, conserves no
// water, and guarantees no monotone channel — the physical sim does all of
// that afterwards. All it does is lower a winding line of cells relative to
// the ground beside them, cheaply, so the real routing has a preferred path.
//
// The rules are deliberately small:
//   - an agent walks downhill-ish from a highland-edge seed, turning gently;
//   - it carves each cell it enters to a fixed depth below its own BANKS, so
//     depth is a function of local terrain only and cannot accumulate along
//     the path;
//   - trails are permanent, and touching ANY trail kills the agent on the spot
//     without carving. That one rule supplies confluences (a tributary ends
//     where it meets a trunk), termination, and islands (an agent looping back
//     into its own trail closes a meander).
//
// See docs/superpowers/specs/2026-07-29-mapgen-canal-precarve-design.md.
//
// Pure function of its inputs apart from the B it mutates — no I/O.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Why an agent stopped. Merged dominating is the HEALTHY outcome: it means the
// network converges rather than running parallel.
enum class CanalEnd : uint8_t { LeftMap, Lake, Merged, StepCap };
inline constexpr int kCanalEndCount = 4;

struct CanalStats {
  int agents = 0;
  int ends[kCanalEndCount] = {0, 0, 0, 0};
  float max_carve_m = 0.0f;
  float total_excavated_m = 0.0f;
  int longest_path = 0;
};

struct CanalResult {
  Field2D<int32_t> trail_source;  // -1 off-canal, else the agent that laid it
  // Compass index (0..7) of the direction flow leaves each trail cell by;
  // kCanalNoDir where the trail ends. Meaningless where trail_source < 0.
  Field2D<uint8_t> trail_dir;
  CanalStats stats;
};

// The compass table trail_dir indexes into, in circular order from +x.
inline constexpr int kCanalDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr int kCanalDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
inline constexpr uint8_t kCanalNoDir = 0xFF;

// Cuts canals into `B`. `lake_mask` marks seeded basins (they absorb and
// attract); `dist_to_plains` supplies the highland edge that seeds agents.
CanalResult carve_canals(Field2D<float>& B, const Field2D<uint8_t>& lake_mask,
                         const Field2D<float>& dist_to_plains,
                         const ErosionParams& p, float texel_m, uint32_t seed);

}  // namespace badlands::mapgen
