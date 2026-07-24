#pragma once

#include <cstdint>
#include <string_view>
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
  // v1.1: texture role only — plains drainage now comes from the base-height
  // relief term in generator.cpp, not from S's noise (was 1.0f).
  float sediment_noise_m = 0.3f;
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
inline constexpr float kMicroFillCapM = 0.75f;  // deepest depression micro_fill may raise

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

// Routes h = B + S once, then raises every closed-depression component of
// `in_lake` cells (4-connected) whose max fill depth (water_level - h) is <=
// kMicroFillCapM AND which contains no basin_mask cell, crediting the fill
// depth to S per member (water_level keeps route_flow's epsilon tilt, so
// filled areas still drain). Deeper depressions and any component touching a
// seeded cavity are left untouched. Deterministic. Returns filled volume
// (m^3).
float micro_fill(Field2D<float>& B, Field2D<float>& S,
                 const Field2D<uint8_t>& basin_mask, float texel_m);

// One implicit Braun–Willett pass in routing order over ground h = B + S.
// K per cell: k_sediment while S > 0 else k_bedrock. On bare bedrock (S == 0)
// the whole eroded depth cuts B at the bedrock rate directly; starting on
// sediment and running out mid-step consumes S first, then re-costs the
// excess at k_bedrock/k_sediment before cutting B.
// in_lake cells are skipped (no floor incision). For a FLOODED receiver
// (in_lake), receiver height is the EFFECTIVE level max(B+S, water_level) so
// donors erode toward the water surface, not the lake floor. For a dry
// receiver, receiver height is just B+S — its in-sweep-updated (already
// eroded) value, since incise() walks r.order and the receiver is processed
// first (Braun–Willett implicit solve). Returns eroded thickness (m) per
// cell.
Field2D<float> incise(Field2D<float>& B, Field2D<float>& S,
                      const FlowRouting& r, const Field2D<float>& area,
                      const ErosionParams& p, float texel_m);

// Route this pass's eroded volume downstream (reverse routing order = donors
// before receivers): each cell receives flux q_in from its donors, deposits
// dep = min(q_in/texel_area, G * q_in / A)   — dry cells
// dep = min(q_in/texel_area, water_level - h) — flooded cells (delta fill)
// into S, then forwards the remainder plus its own erosion. Flux reaching
// base-level cells (receiver -1) leaves the map. Returns exported volume m³.
float deposit(Field2D<float>& B, Field2D<float>& S,
              const Field2D<float>& eroded_m, const FlowRouting& r,
              const Field2D<float>& area, const ErosionParams& p,
              float texel_area_m2);

// Explicit 5-point diffusion of h = B + S on interior cells (border pinned),
// sub-stepped so D*dt_sub/texel² <= 0.25. Negative dh draws S before B;
// positive dh credits S (diffused material is loose).
void diffuse(Field2D<float>& B, Field2D<float>& S, const ErosionParams& p,
             float texel_m);

// Debug/preview sink for the erosion sim: dumps named raster stages as the
// loop runs. stages: "loop-height", "loop-flow", "loop-sediment" (float);
// "loop-lakes" (uint8). Later tasks add init/output stages.
struct MapDebugSink {
  virtual ~MapDebugSink() = default;
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<float>& field) = 0;
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<uint8_t>& mask) = 0;
};

struct ErosionOutputs {
  Field2D<float> water_depth;  // m of standing water after pruning
  Field2D<float> flow;         // final drainage area (m²)
};

// The full sim: iterations × (route → drain → incise → deposit → diffuse),
// then a final route to flood lakes, measure spill levels, and prune lakes
// under min area/depth. Mutates B and S. sink may be null.
ErosionOutputs erode(Field2D<float>& B, Field2D<float>& S,
                     const ErosionParams& p, float texel_m,
                     MapDebugSink* sink);

}  // namespace badlands::mapgen
