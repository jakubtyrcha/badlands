#pragma once

// The ALGORITHMS that build and de-lattice the river network: extraction from
// the flow routing, the Manning + regime-width solve, and the polyline helpers.
//
// The network's data TYPES live in mapgen/river_network.hpp. They are separated
// because extraction needs the routing and a Field2D, so including this header
// costs a consumer hydrology.hpp and field2d.hpp — a price a consumer that only
// wants to hold a graph should not pay.
//
// This replaces the old per-texel `river` intensity raster (a 0..1 smoothstep
// over log2(drainage area)), which had no physical meaning, fragmented at every
// pruned micro-depression, and could not survive resampling. See
// docs/superpowers/specs/2026-07-29-mapgen-river-network-design.md.
//
// Pure functions of their inputs — no I/O, no failure path.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/hydrology.hpp"
#include "mapgen/river_network.hpp"

namespace badlands::mapgen {

// --- hydraulics -------------------------------------------------------------

// Slope is floored at kMinChannelSlope: Manning divides by sqrt(S), so a truly
// flat reach would give infinite depth. Q <= 0 returns all zeros. The solve
// itself is documented on ChannelHydraulics (river_network.hpp).
inline constexpr float kMinChannelSlope = 1e-4f;

ChannelHydraulics channel_hydraulics(float discharge_m3_s, float slope,
                                     const ErosionParams& p);

// Monotone non-decreasing in discharge. Any Q > 0 is at least Rill, so a
// rasterized channel never classifies as None.
RiverClass classify_discharge(float discharge_m3_s);

// --- lake components ---------------------------------------------------------

// 4-connected components of the cells for which `flag[i]` is truthy, returned
// in the order their lowest-index member is discovered — deterministic. Used
// by extract_river_graph to label the wet cells it treats as lake nodes.
std::vector<std::vector<int>> label_lake_components(int w, int ht,
                                                    const std::vector<uint8_t>& flag);

// --- extraction ---------------------------------------------------------------

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

// --- polyline helpers (exposed for tests) -----------------------------------

// Douglas-Peucker simplification, keeping endpoints. Removes the D8 staircase
// while preserving genuine turns.
std::vector<glm::vec2> simplify_polyline(const std::vector<glm::vec2>& pts,
                                         float tolerance_m);

// Resample to roughly uniform arc length, keeping both endpoints. Gives
// consumers evenly spaced vertices for curvature estimation and for the carve.
std::vector<glm::vec2> resample_polyline(const std::vector<glm::vec2>& pts,
                                         float spacing_m);

// --- conservative-rasterization helpers (shared, not just for tests) --------
//
// Exposed because the conservative rule is a CONTRACT the carve pass has to
// honour too (river_carve.cpp rasterizes the arc chains against the same texel
// squares, one chord at a time). A second copy of this geometry would be a
// second chance to get the "square, not centre" test subtly wrong.

// Does the segment ab touch the axis-aligned box at all? Liang-Barsky slab clip.
// Exported alongside segment_aabb_distance (which uses it) rather than for its
// own sake -- river_carve.cpp calls only the distance. Kept declared because
// "do they touch" and "how far apart" are one predicate split in two, and
// hiding half of it invites the next caller to rewrite it.
bool segment_hits_aabb(glm::vec2 a, glm::vec2 b, glm::vec2 lo, glm::vec2 hi);

// Exact minimum distance between a segment and an axis-aligned box (0 when they
// intersect). For two convex sets the minimum is attained at a vertex of one
// against the other, so the box's corners against the segment plus the
// segment's endpoints against the box cover every case.
float segment_aabb_distance(glm::vec2 a, glm::vec2 b, glm::vec2 lo,
                            glm::vec2 hi);

}  // namespace badlands::mapgen
