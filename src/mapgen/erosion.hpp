#pragma once

#include <cstdint>
#include <vector>
#include "mapgen/field2d.hpp"
#include "mapgen/hydrology.hpp"

namespace badlands::mapgen {

struct ErosionParams {
  int sim_resolution = 512;   // sim grid texels (square, excl. pad)
  int iterations = 80;
  float dt = 1.0f;            // nominal time unit
  float m = 0.5f;             // stream-power area exponent (slope exponent n fixed at 1)
  float k_sediment = 5e-3f;
  float k_bedrock = 5e-4f;
  float deposition_g = 1.0f;
  float diffusion = 0.02f;    // D (m²/dt)
  float initial_sediment_m = 4.0f;
  float sediment_taper_m = 60.0f;
  float sediment_noise_m = 1.0f;
  float sediment_noise_wavelength_m = 40.0f;
  float lake_frac = 0.03f;
  float lake_depth_m = 12.0f;
  float min_lake_area_m2 = 400.0f;
  float min_lake_depth_m = 0.5f;
  int dump_every = 10;        // loop dump cadence (0 = off)
  int detail_octaves = 4;
  float detail_wavelength_m = 60.0f;
  float detail_amplitude_m = 2.0f;
};

inline constexpr int kPadTexels = 16;      // sim-grid margin, cropped on output
inline constexpr float kEpsilonM = 1e-4f;  // flood epsilon per step

// Subtract smooth cavity bowls where sim-grid bedrock is in its bottom
// lake_frac quantile. Depth grows quadratically from the quantile rim to
// lake_depth_m at the bedrock minimum. Returns the basin mask (1 = carved).
Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float lake_depth_m);

// S0 = initial_sediment_m * clamp(1 - d/taper, 0, 1) + fBm noise, clamped >= 0;
// zero inside basins. Noise is sampled at world meters (x * texel_m + origin_m)
// with seed+3 (seeds 0..2 are taken by base/ridged/belt).
Field2D<float> init_sediment(const Field2D<float>& dist_to_plains,
                             const Field2D<uint8_t>& basin_mask,
                             const ErosionParams& p, float texel_m,
                             float origin_m, uint32_t seed);

// One implicit Braun–Willett pass in routing order over ground h = B + S.
// K per cell: k_sediment while S > 0 else k_bedrock; eroded depth consumes S
// first, the excess rescaled by k_bedrock/k_sediment before cutting B.
// in_lake cells are skipped (no floor incision). Receiver height is the
// EFFECTIVE level max(B+S, water_level) so cells erode toward the water
// surface, not a lake floor. Returns eroded thickness (m) per cell.
Field2D<float> incise(Field2D<float>& B, Field2D<float>& S,
                      const FlowRouting& r, const Field2D<float>& area,
                      const ErosionParams& p, float texel_m);

}  // namespace badlands::mapgen
