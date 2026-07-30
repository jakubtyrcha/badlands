#pragma once

// D8 flow routing over a heightfield, in TWO passes:
//
//   1. Depression filling by priority-flood (Barnes, Lehman & Mulla 2014): a
//      single flood-fill from the border inward, breaking ties with an epsilon
//      gradient, producing the filled surface `water_level`, the `in_lake`
//      flags, and the topological pop `order`.
//   2. Flow DIRECTION by steepest descent on that filled surface: each cell's
//      receiver maximizes (drop / horizontal distance) over its strictly-lower
//      neighbors.
//
// The second pass is not optional bookkeeping. Priority-flood's claim parent
// ranks candidates by absolute elevation and ignores the distance to the
// neighbor, and a diagonal is always at least as low as the best orthogonal —
// so using the claim parent as the receiver drains every interior cell
// diagonally and locks the network to 45 degrees. See route_flow's definition
// for the measurements and the invariants pass 2 must preserve.
//
// Border cells are base level (receiver -1); all flow exits through them.
// Incision/deposition consume `order` and `accumulate_drainage`'s per-cell
// contributing area.
//
// Pure function of the heightfield — no I/O, no failure path.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// D8 receiver graph over the epsilon-filled surface. Border cells are base
// level: receiver -1, all flow exits through them. Deterministic: both the
// flood's priority ties and the steepest-descent ties break on linear index.
struct FlowRouting {
  int width = 0, height = 0;
  // Linear idx of the STEEPEST-DESCENT receiver on `water_level` (-1 =
  // border/base level). Not the priority-flood claim parent — see the header
  // note above.
  std::vector<int32_t> receiver;
  std::vector<int32_t> order;      // pop order; a cell's receiver pops before it (topological)
  std::vector<uint8_t> in_lake;    // 1 = flooded above ground (depression interior)
  std::vector<float> water_level;  // flood level for in_lake cells, else ground height
};

// Floods `h` from the border inward (8-connected), leveling flooded
// depressions and flats by `epsilon_m` per BFS step, then assigns each
// interior cell the neighbor maximizing (drop / horizontal distance) on the
// filled surface. Receivers strictly descend that surface, so the graph is
// acyclic and `order` stays topological. `texel_m` sets the orthogonal vs
// diagonal step distance in the descent ranking.
//
// `lake_tag` (optional, sim-grid sized) marks cells belonging to a RESOLVED
// lake; those keep the flood tree instead of taking a steepest-descent
// receiver. Pass null to fall back on `in_lake`, which also treats every flat
// as a lake and so routes a large share of channels by flood order rather than
// gradient. The tag must mark whole lakes, not merely their deep cells — see
// route_flow's definition for why a shallow untagged margin is unsafe.
//
// Taking the tag as an INPUT rather than deriving it here is deliberate:
// resolving lakes needs catchment area, which needs the receiver graph, which
// needs this exclusion. Supplying it externally breaks that cycle.
FlowRouting route_flow(const Field2D<float>& h, float texel_m, float epsilon_m,
                       const Field2D<uint8_t>* lake_tag = nullptr);

// Per-cell contributing drainage area (m^2): every cell starts with
// `texel_area_m2` of its own rain and accumulates from all cells that name
// it as receiver, walked in reverse topological (leaf-to-root) order.
Field2D<float> accumulate_drainage(const FlowRouting& r, float texel_area_m2);

}  // namespace badlands::mapgen
