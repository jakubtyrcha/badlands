#pragma once

// D8 flow routing over a heightfield via priority-flood (Barnes, Lehman &
// Mulla 2014): a single flood-fill from the border inward, breaking ties with
// an epsilon gradient so every cell — including flats and depression
// interiors — gets a strictly downhill (or flat-by-epsilon) receiver. Border
// cells are base level (receiver -1); all flow exits through them. This is
// the flow-routing foundation for the stream-power erosion sim (later
// tasks): incision/deposition consume `route_flow`'s topological `order` and
// `accumulate_drainage`'s per-cell contributing area.
//
// Pure function of the heightfield — no I/O, no failure path.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// D8 receiver graph from priority-flood (Barnes 2014) with an epsilon
// gradient across flats and flooded depressions. Border cells are base
// level: receiver -1, all flow exits through them. Deterministic: priority
// ties break on linear index.
struct FlowRouting {
  int width = 0, height = 0;
  std::vector<int32_t> receiver;   // linear idx of downhill receiver; -1 = border/base level
  std::vector<int32_t> order;      // pop order; a cell's receiver pops before it (topological)
  std::vector<uint8_t> in_lake;    // 1 = flooded above ground (depression interior)
  std::vector<float> water_level;  // flood level for in_lake cells, else ground height
};

// Floods `h` from the border inward (8-connected), assigning each interior
// cell the neighbor it was reached from as its receiver. Flooded depressions
// and flats are leveled by `epsilon_m` per BFS step so the graph stays
// strictly acyclic. `texel_m` is unused today (reserved for a future
// distance-weighted epsilon) — kept in the signature so callers don't need
// to change when that lands.
FlowRouting route_flow(const Field2D<float>& h, float texel_m, float epsilon_m);

// Per-cell contributing drainage area (m^2): every cell starts with
// `texel_area_m2` of its own rain and accumulates from all cells that name
// it as receiver, walked in reverse topological (leaf-to-root) order.
Field2D<float> accumulate_drainage(const FlowRouting& r, float texel_area_m2);

}  // namespace badlands::mapgen
