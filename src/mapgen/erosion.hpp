#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>
#include "mapgen/field2d.hpp"
#include "mapgen/hydrology.hpp"

namespace badlands::mapgen {

struct ErosionParams {
  int sim_resolution = 512;   // sim grid texels (square, excl. pad)
  // v1.2: iterations x dt is the age product; the implicit solve is stable at
  // large dt, so fewer/bigger steps trade only drainage re-adaptation. 80x1 ->
  // 40x2 measured near-identical (heightmap delta <= 28/255, water mask 1
  // texel); user judged the 10x8 ladder rung by preview and picked it.
  int iterations = 10;
  float dt = 8.0f;            // nominal time unit
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
  // v1.3: 0.03 -> 0.08, user-directed ("increase lake noise threshold by 5%",
  // read as +5 percentage points of basin coverage) — see the v1.3 addendum,
  // "Lake tuning", in
  // docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md.
  float lake_frac = 0.08f;
  float min_lake_area_m2 = 400.0f;
  float min_lake_depth_m = 0.5f;
  // Outlet notch depth for SEEDED basins. DISABLED (0) by default: the
  // mechanism is exact on synthetic bowls but inert on production terrain,
  // because seeded basins sit at bedrock minima out in the plains, where total
  // relief is only kPlainsReliefM (2 m) and there is no lower ground for a
  // channel to reach. Measured on seed 2: 100% of notch channels hit
  // kMaxNotchSteps without ever meeting ground below them, so every one ends
  // in a pit that floods with the bowl and leaves the level unmoved (9492 vs
  // 9540 wet texels with and without). See carve_outlet_notches.
  float notch_depth_m = 0.0f;
  int dump_every = 10;        // loop dump cadence (0 = off)
  int detail_octaves = 4;
  float detail_wavelength_m = 60.0f;
  float detail_amplitude_m = 2.0f;
  // --- river network (v2: graph, not intensity raster) ---
  // See docs/superpowers/specs/2026-07-29-mapgen-river-network-design.md.
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
  // Lake freeboard: the output water level sits below the spill point so a dry
  // bank of already-carved bowl is exposed and the Lake biome covers only
  // water. Applied at finalize only, so the sim loop is unaffected. The
  // fractional cap stops shallow ponds from vanishing outright.
  float lake_freeboard_m = 0.4f;
  float lake_freeboard_frac = 0.25f;
};

inline constexpr int kPadTexels = 16;      // sim-grid margin, cropped on output
inline constexpr float kEpsilonM = 1e-4f;  // flood epsilon per step
inline constexpr float kMicroFillCapM = 0.75f;  // deepest depression micro_fill may raise
// v1.3.1: basins touching the sim-grid border are breached by construction —
// border cells seed route_flow's priority-flood at their own (possibly
// carved-to-basin-floor) height, so a basin whose mask reaches the border
// "spills" through the border at bowl-floor height instead of holding water.
// carve_cavities excludes this many texels of ring at the sim-grid edge from
// the basin mask before carving, so the ring keeps its original terrain
// height and acts as a dam; the EDT cone then shallows naturally toward it.
// See docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md,
// "Basin border rim (v1.3.1)".
inline constexpr int kBasinBorderMarginTexels = 3;

// Outlet-notch channel limits. The notch must run until it meets ground
// already below it — lowering only the sill would leave a pit, which floods
// together with the bowl and restores the original spill height. The cap
// bounds how far a channel may trench when the downstream slope is very
// gentle; hitting it means the notch only partially lowered the lake.
inline constexpr int kMaxNotchSteps = 24;
inline constexpr float kNotchStepDropM = 0.02f;

// 4-connected components of the cells for which `flag[i]` is truthy, returned
// in the order their lowest-index member is discovered — deterministic.
// Used for lake components by micro_fill, deposit's lake pour, and the river
// graph's lake nodes.
std::vector<std::vector<int>> label_lake_components(int w, int ht,
                                                    const std::vector<uint8_t>& flag);

// Carve the bottom lake_frac quantile of bedrock into inverted-cone basins:
// depth = slope_m_per_m * (exact EDT world-meter distance to the nearest
// NON-basin texel). Depth scales with basin size (uncapped), mirroring the
// mountain cone at the caller's slope. Returns the basin mask.
Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float slope_m_per_m,
                                glm::vec2 texel_m, float notch_depth_m = 0.0f);

// Cut an outlet notch for each basin component: find the lowest cell just
// outside it (where it would spill anyway) and lower that cell, plus a
// one-texel neighbourhood, by notch_depth_m.
//
// This is what gives a seeded lake a COAST. A bowl fills to its lowest rim
// point, and carve_cavities zeroes the cone exactly at the mask boundary, so
// the rim is the surrounding terrain and water reaches it everywhere — no bank
// exists by construction. Lowering the water instead would be unphysical (a
// real balance says these lakes fill to the brim); lowering one rim point is
// what a real lake with an outlet river actually looks like. Routing needs no
// change: priority-flood still finds the true spill, it is simply lower.
//
//     bank_width_m = notch_depth_m / basin_cone_slope
//
// so at the production cone slope (0.5 m/m) a 1 m notch yields a 2 m bank.
// Judge the depth by the coast width wanted, not by the depth alone.
void carve_outlet_notches(Field2D<float>& B, const Field2D<uint8_t>& mask,
                          float notch_depth_m);

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
// before receivers).
// DRY cells: deposit dep = min(q_in/texel_area, G * q_in / A) into S, then
// forward the remainder plus this cell's own erosion to its receiver. Flux
// reaching base-level cells (receiver -1) leaves the map.
// LAKE cells (in_lake): no local deposit and no forwarding during the sweep
// — instead every in_lake cell's (q_in + own eroded volume) is added to its
// 4-connected lake component's bucket. After the sweep, components are
// poured in deterministic order (descending pop-order of each component's
// deepest — last-flooded — member, so an upstream lake resolves before a
// downstream lake it may cascade into): each bucket fills bottom-up (lowest
// ground first, ties by linear index; a shared pool level rises, capped at
// the component's water level). This replaces per-cell entry deposition,
// which produced visible delta stripes along D8 chains. Volume left over
// once the whole component reaches water level walks the component's single
// outlet down the receiver chain under the normal dry rule; if that walk
// re-enters an unpoured lake component it merges into that component's
// bucket (resolved on its own turn), and if it enters an already-poured
// component it passes through untouched to that component's own outlet.
// Whatever reaches base level exports. Conservation (deposited + exported ==
// eroded) holds exactly as before. Returns exported volume m³.
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
  // The FINAL routing, kept so the river graph can be extracted from exactly
  // the network the outputs above were measured on. Re-routing downstream
  // would risk a subtly different graph for no benefit.
  FlowRouting routing;
};

// The full sim: iterations × (route → drain → incise → deposit → diffuse),
// then a final route to flood lakes, measure spill levels, and prune lakes
// under min area/depth. The river NETWORK is no longer built here — see
// river_graph.hpp, which extracts it from the final routing. Mutates B and
// S. sink may be null.
ErosionOutputs erode(Field2D<float>& B, Field2D<float>& S,
                     const ErosionParams& p, float texel_m,
                     MapDebugSink* sink);

}  // namespace badlands::mapgen
