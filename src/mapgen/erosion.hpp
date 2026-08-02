#pragma once

// Channel-hydraulics parameters: what is left of the erosion sim's parameter
// block now that the sim itself is gone. Six fields, every one of them still
// read by the surviving river code (river_graph.cpp, window_rivers.cpp).
//
// The lake record moved to mapgen/lake.hpp. This header now includes NOTHING --
// it previously pulled field2d.hpp, hydrology.hpp and glm without using any of
// them, which is what stopped the graph types from being cheap to include.
//
// The name is historical and wrong; renaming it reaches into enough call sites
// to be its own change.

namespace badlands::mapgen {

struct ErosionParams {
  // --- river network ---
  // Drainage area at which a cell counts as a channel and enters the graph.
  float min_channel_area_m2 = 1500.0f;
  // Runoff depth per second. ~1 m/year of runoff for a temperate basin, kept
  // PHYSICALLY HONEST rather than inflated to make rivers look bigger: at this
  // world scale a 512 m map drains at most ~0.0025 m^3/s, so these are creeks,
  // and the renderer conveys size by class rather than by width. Inflating it
  // would make every derived unit fictional.
  float runoff_m_per_s = 3.17e-8f;
  // Regime width closure w = channel_width_coeff * sqrt(Q); mid-range for
  // natural channels.
  float channel_width_coeff = 5.0f;
  float manning_n = 0.035f;  // natural channel with some bed roughness
  // Polyline de-latticing: Douglas-Peucker tolerance and resample spacing, in
  // TEXELS (scaled by texel_m at use), since the staircase they remove is a
  // lattice artifact and so scales with the grid, not the world.
  float simplify_tolerance_texels = 0.9f;
  float resample_spacing_texels = 3.0f;
};

inline constexpr float kEpsilonM = 1e-4f;  // flood epsilon per step

}  // namespace badlands::mapgen
