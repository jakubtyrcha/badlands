#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "mapgen/biomes.hpp"

namespace badlands::mapgen {

// All runtime-tunable knobs for a mapgen run, with defaults. Loaded from an
// optional JSON config file (load_config) and overridden by CLI flags in main.
//
// NOTE: sample density (1 m) and block size (10 m) are NOT here — they are the
// compile-time structural constants in mapgen_constants.hpp. Only the overall
// map size (width/height, in samples = meters) is runtime.
struct MapgenConfig {
  uint32_t seed = 1;
  // 512x512 m for now: fast to generate + view while the terrain work is
  // iterating, and a whole number of 4 m blocks (128) so no remainder samples
  // are dropped. The 2 km target is a --resolution/--config away, not a rebuild.
  int width = 512;   // samples on X (meters at 1 m density)
  int height = 512;  // samples on Y
  std::string out_dir = "mapgen_out";

  // --- Voronoi pre-sections ---
  float cell_size_m = 130.0f;  // average voronoi cell edge (meters)
  float seed_jitter = 0.85f;   // 0..1 jitter of seeds within their grid cell

  // --- Noise fields (frequencies in cycles across the whole map) ---
  float elevation_freq = 2.0f;
  float moisture_freq = 3.0f;
  float ridged_freq = 8.0f;
  float fine_freq = 24.0f;

  // --- Biome assignment (on normalized elevation/moisture in [0,1]) ---
  float elev_lake = 0.30f;      // cluster elevation below this -> Lake
  float elev_high = 0.62f;      // cluster elevation above this -> Hills
  float moisture_wet = 0.50f;   // moisture above this -> wet side

  // --- Height composition (world meters) ---
  float height_scale_m = 14.0f;   // span from base elevation field
  float cavity_depth_m = 2.5f;    // extra sink for lake/swamp basins
  float hills_ridge_m = 6.0f;     // ridge amplitude folded into hills relief
  // Terrace quantization: the composed relief is snapped to levels this far
  // apart (meters), giving mesa/plateau structure so the flat sections sit at
  // distinct heights. Must exceed section_step_m (+ variation) for terraces to
  // read as separate sections. Set 0 to disable (smooth terrain).
  float terrace_step_m = 1.2f;
  // Small natural variation kept within a region so sections aren't dead-flat.
  // Indexed by Biome, all kept below section_step_m so they never split a
  // terrace. Hills get the most; lake/swamp the least.
  std::array<float, kBiomeCount> variation_amp_m = {
      0.08f,  // Lake
      0.10f,  // Swamp
      0.18f,  // Forest
      0.15f,  // Plains
      0.28f,  // Hills
      0.32f,  // Mountain
  };

  // --- Section extraction ---
  float section_step_m = 0.5f;  // neighbor block |Δheight| above this splits
                                // sections (a ledge)
  int min_section_blocks = 6;   // sections smaller than this merge into a
                                // neighbor
  bool reduce_median = true;    // block height = median (true) or mean (false)
                                // of its footprint
};

// Load config from a JSON file, overriding defaults for keys that are present.
// An empty path or an unreadable file returns defaults (a warning to stderr).
MapgenConfig load_config(const std::string& path);

}  // namespace badlands::mapgen
