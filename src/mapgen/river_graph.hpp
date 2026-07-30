#pragma once

// The river network as a GRAPH, extracted from the flow routing, plus the
// conservative rasterization of that graph into physical output fields.
//
// This replaces the old per-texel `river` intensity raster (a 0..1 smoothstep
// over log2(drainage area)), which had no physical meaning, fragmented at every
// pruned micro-depression, and could not survive resampling. See
// docs/superpowers/specs/2026-07-29-mapgen-river-network-design.md.
//
// Direction is structural, not conventional: every cell has exactly one
// receiver and `order` is topological, so the channel subgraph is a directed
// forest — an edge cannot flow both ways and a cycle cannot form.
//
// Pure functions of their inputs — no I/O, no failure path.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/hydrology.hpp"

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

// Slope is floored at kMinChannelSlope: Manning divides by sqrt(S), so a truly
// flat reach would give infinite depth. Q <= 0 returns all zeros.
inline constexpr float kMinChannelSlope = 1e-4f;

ChannelHydraulics channel_hydraulics(float discharge_m3_s, float slope,
                                     const ErosionParams& p);

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

// Monotone non-decreasing in discharge. Any Q > 0 is at least Rill, so a
// rasterized channel never classifies as None.
RiverClass classify_discharge(float discharge_m3_s);

// --- graph ------------------------------------------------------------------

enum class RiverNodeKind : uint8_t {
  Source,      // channel head: no upstream channel donor
  Confluence,  // two or more channel donors meet
  LakeInlet,   // a channel terminates at a lake shore
  LakeOutlet,  // a lake's spill point, where its outflow begins
  Mouth,       // leaves the map at base level
};

struct RiverNode {
  glm::vec2 pos_m{0.0f};  // WORLD METERS, float — deliberately not a texel index
  float ground_m = 0.0f;
  float drainage_area_m2 = 0.0f;
  float discharge_m3_s = 0.0f;
  float width_m = 0.0f, depth_m = 0.0f, speed_m_s = 0.0f;
  int32_t lake_id = -1;  // >= 0 for LakeInlet / LakeOutlet
  // Only meaningful when lake_id >= 0. A Seeded lake is a deliberate map
  // feature placed by carve_cavities; an Emergent one is whatever the sim
  // happened to pond.
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

// Builds the graph from the FINAL routing over the sim grid.
//
// `water_depth` must be the PRUNED field (post finalize_lakes), not
// `r.in_lake`. That difference is the whole fix for the old fragmentation:
// river_intensity zeroed every in_lake cell, including the ~11% belonging to
// depressions that pruning had already turned back into dry land, punching the
// network full of gaps with no visible lake to explain them.
//
// `ground` is B + S on the sim grid, used for node elevations and reach slopes.
// Positions are world meters: texel (x, y) sits at (x*texel_m + origin_m, ...).
// `lake_id` / `lakes` (optional) let lake nodes report which lake they belong
// to and whether it was seeded or emergent. Passing null leaves lake_kind at
// its default; the graph is otherwise unchanged.
RiverGraph extract_river_graph(const FlowRouting& r, const Field2D<float>& area,
                               const Field2D<float>& water_depth,
                               const Field2D<float>& ground,
                               const ErosionParams& p, float texel_m,
                               float origin_m,
                               const Field2D<int32_t>* lake_id = nullptr,
                               const std::vector<LakeInfo>* lakes = nullptr);

// --- rasterization ----------------------------------------------------------

// The per-texel fields the graph rasterizes into. Discharge, depth and speed
// are reach attributes splatted onto the texels that reach covers — none is a
// true point quantity, and treating discharge differently from the other two
// would be arbitrary. Cross-section (w*d) and width stay on the graph: they are
// through-flow across a SECTION, so a per-texel value is meaningless, and width
// is recoverable anyway as k_w*sqrt(Q).
struct RiverRasters {
  Field2D<float> discharge_m3_s;
  Field2D<uint8_t> cls;  // RiverClass values
  Field2D<float> depth_m;
  Field2D<float> speed_m_s;
  Field2D<glm::vec2> flow_dir;  // unit; (0,0) where there is no channel
};

// CONSERVATIVE rasterization: a texel is covered when its SQUARE overlaps the
// segment's capsule, not when its centre does. That guarantees the covered set
// of an edge is connected end to end at any resolution — even for a channel
// narrower than a texel, since the axis still passes through them. Centre
// sampling plus dilation plus max-pooling (what this replaces) could not.
//
// Where reaches overlap the higher-discharge one wins, so a trunk beats its
// tributary at a confluence. Rasterizing at output resolution from world-space
// geometry makes resolution independence free.
RiverRasters rasterize_rivers(const RiverGraph& g, int res, float texel_m);

// --- polyline helpers (exposed for tests) -----------------------------------

// Douglas-Peucker simplification, keeping endpoints. Removes the D8 staircase
// while preserving genuine turns.
std::vector<glm::vec2> simplify_polyline(const std::vector<glm::vec2>& pts,
                                         float tolerance_m);

// Resample to roughly uniform arc length, keeping both endpoints. Gives
// consumers evenly spaced vertices for curvature estimation and for the future
// carve pass.
std::vector<glm::vec2> resample_polyline(const std::vector<glm::vec2>& pts,
                                         float spacing_m);

}  // namespace badlands::mapgen
