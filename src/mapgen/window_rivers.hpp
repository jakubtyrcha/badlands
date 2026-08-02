#pragma once

// Builds the river network for a WINDOWED map — a cutout of a larger simulated
// world, loaded by map_io.
//
// The whole chain already exists (route_flow -> accumulate_drainage ->
// extract_river_graph -> channel_hydraulics); the one thing a window needs that
// a whole map does not is BOUNDARY INFLOW.
//
// A cutout's rivers cross into it carrying discharge from ground it cannot see.
// Routing the window alone would start every channel at zero on the edge, so the
// trunk river arrives as a trickle and its width, depth and speed are all wrong.
// The parent map knows the discharge at each crossing, so each is converted to
// the upstream catchment it implies:
//
//     A_in = Q_in / runoff
//
// and seeded at the entry cell. Expressing inflow as an AREA rather than a
// discharge is what lets it ride the existing area accumulation untouched, and
// the discharge that comes out downstream is consistent with the parent map's by
// construction rather than by a second conversion.
//
// Pure function of its inputs — no I/O, no failure path.

#include <string>
#include <vector>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

// One river crossing the window boundary, at OUTPUT texel resolution.
struct RiverInflow {
  int texel_x = 0, texel_y = 0;
  float discharge_m3_s = 0.0f;
};

// Reads `<dir>/inflows.txt` as written by tools/protogen/window.cpp. A missing
// file is NOT an error — it means no river crosses the boundary, which is a
// legitimate map — so this returns an empty vector for both. `runoff_m_per_yr`,
// if non-null, receives the value the file records, so the caller can check it
// against the erosion params rather than assume they agree.
std::vector<RiverInflow> load_inflows(const std::string& dir,
                                      float* runoff_m_per_yr = nullptr);

struct WindowRivers {
  RiverGraph graph;
  Field2D<float> drainage_area_m2;  // includes the seeded upstream inflow
  FlowRouting routing;
  // Totals, for reporting and for checking the window against its parent.
  float inflow_m3_s = 0.0f;   // sum of the boundary crossings
  float rain_m3_s = 0.0f;     // runoff over the window's own area
};

// Routes the window, accumulates drainage with `inflows` seeded as upstream
// area, and extracts the river graph. `art` must carry heightmap, water_depth,
// lake_id and lakes (map_io fills all four).
//
// Lakes are passed to route_flow as a tag so their cells keep the flood tree
// instead of taking steepest descent — without it a flat lake surface invents
// downhill exits through its own rim.
//
// `min_channel_width_m` and `min_branch_length_m` prune the result via
// river_prune.hpp (prune_river_graph_by_width / prune_river_graph_by_length);
// 0 keeps everything. Width is applied first, since it trims reaches back to
// where they qualify and so decides what a reach's length even is.
WindowRivers build_window_rivers(const MapArtifacts& art, float world_size_m,
                                 const std::vector<RiverInflow>& inflows,
                                 const ErosionParams& p,
                                 float min_channel_width_m = 0.0f,
                                 float min_branch_length_m = 0.0f);

}  // namespace badlands::mapgen
