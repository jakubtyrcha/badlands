#pragma once

// The river network's DATA TYPES -- the graph, its nodes and reaches, the
// discharge class, and the solved channel cross-section.
//
// Split out of river_graph.hpp, which holds the ALGORITHMS that produce and
// consume them (extraction, hydraulics, polyline de-latticing). The split is
// not cosmetic: extraction needs the flow routing and a Field2D, so
// river_graph.hpp transitively costs a consumer hydrology.hpp and field2d.hpp.
// A consumer that only wants to HOLD a network -- the patch contract, a
// serializer, a renderer -- should pay for none of that. This header includes
// glm, <cstdint>, <vector> and lake.hpp, and nothing else.
//
// Direction is structural, not conventional: every cell has exactly one
// receiver and the routing order is topological, so the channel subgraph is a
// directed forest -- an edge cannot flow both ways and a cycle cannot form.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/lake.hpp"

namespace badlands::mapgen {

// --- hydraulics -------------------------------------------------------------

// Channel geometry and velocity at one point, solved rather than assumed.
// Specifying depth and velocity independently would over-determine the system
// and break continuity; these come from Manning plus a regime width closure,
// solved together:
//
//   w = k_w * sqrt(Q)                        regime width (exponent b = 0.5)
//   v = (1/n) * d^(2/3) * sqrt(S)            Manning, wide channel (R ~= d)
//   Q = w * d * v                            continuity
//     =>  d = ( Q*n / (w*sqrt(S)) )^(3/5),  v = Q / (w*d)
//
// The implied exponents are w ~ Q^0.5, d ~ Q^0.3, v ~ Q^0.2, summing to 1.0, so
// continuity holds identically rather than approximately. At fixed Q a steeper
// reach is shallower and faster (rapids); a flat reach is deeper and slower.
struct ChannelHydraulics {
  float width_m = 0.0f;
  float depth_m = 0.0f;
  float speed_m_s = 0.0f;
};

// --- river class ------------------------------------------------------------

// Absolute log-decade discharge tiers. Absolute rather than per-map quantiles
// so a class means the same flow on every map: a 512 m map only ever reaches
// Stream, while a 10 km map promotes reaches into the upper tiers naturally.
// 0 (None) doubles as "no channel here".
enum class RiverClass : uint8_t {
  None = 0,
  Rill,    // < 1e-4 m^3/s
  Brook,   // 1e-4 .. 1e-3
  Stream,  // 1e-3 .. 1e-2
  Creek,   // 1e-2 .. 1e-1
  River,   // 1e-1 .. 1e0
  Major,   // >= 1e0
};
inline constexpr int kRiverClassCount = 7;

// --- graph ------------------------------------------------------------------

enum class RiverNodeKind : uint8_t {
  Source,      // channel head: no upstream channel donor
  Confluence,  // two or more channel donors meet
  LakeInlet,   // a channel terminates at a lake shore
  LakeOutlet,  // a lake's spill point, where its outflow begins
  Mouth,       // leaves the map at base level
  // Inbound twin of Mouth: a reach that ENTERS this region from outside it,
  // whose upstream continues beyond the frame. `in_deg == 0` alone cannot
  // distinguish this from a genuine headwater (Source) -- a trunk that merely
  // crosses into a window has no in-frame upstream either -- so clipping mints
  // this kind explicitly rather than leaving it to be re-derived. Appended
  // after Mouth so existing numeric/serialized values do not shift.
  FrameEntry,
};

struct RiverNode {
  glm::vec2 pos_m{0.0f};  // WORLD METERS, float — deliberately not a texel index
  float ground_m = 0.0f;
  float drainage_area_m2 = 0.0f;
  float discharge_m3_s = 0.0f;
  float width_m = 0.0f, depth_m = 0.0f, speed_m_s = 0.0f;
  int32_t lake_id = -1;  // >= 0 for LakeInlet / LakeOutlet
  // Only meaningful when lake_id >= 0. A Seeded lake is a deliberate map
  // feature; an Emergent one is whatever the sim happened to pond.
  LakeKind lake_kind = LakeKind::Emergent;
  RiverNodeKind kind = RiverNodeKind::Source;
};

// A directed reach. Flow is ALWAYS from `from` to `to`.
//
// `points_m` covers only the DRY portion: an edge terminating at a lake stops
// at the shoreline, because the lake surface is already water and drawing a
// channel across it would be wrong. The lake connection survives in the
// topology (`to` names the lake's node) even though no geometry crosses it.
//
// Vertices are world-space floats at variable arc length, NOT one per texel.
// The chain is walked cell-by-cell, then simplified and resampled — D8 offers
// only 8 headings, so a raw chain staircases at every heading off the lattice
// (sinuosity up to 1.0824, peaking at exactly 22.5 degrees). The true heading
// is recoverable because the orthogonal:diagonal step RATIO encodes it, which
// is what the simplification extracts.
struct RiverEdge {
  int32_t from = -1, to = -1;
  std::vector<glm::vec2> points_m;
  std::vector<float> discharge_m3_s;  // per point
  std::vector<float> width_m, depth_m, speed_m_s;
  // Hierarchy, constant along a reach. Strahler is the render/gameplay tier
  // (leaves 1; equal child orders i give i+1, else the max). Shreve is
  // additive and equals the number of upstream sources. They differ where a
  // low-order tributary joins a trunk: Strahler holds, Shreve increments.
  int32_t strahler_order = 1;
  int32_t shreve_magnitude = 1;
};

struct RiverGraph {
  std::vector<RiverNode> nodes;
  std::vector<RiverEdge> edges;
};

}  // namespace badlands::mapgen
