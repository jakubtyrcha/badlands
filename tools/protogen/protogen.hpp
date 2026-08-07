#pragma once

// The shared surface between protogen's TUs (protogen.cpp: phase-0 + driver;
// protogen_swe.cpp: the phase-1 SWE fluid passes; protogen_tests.cpp: the
// sanity-test suite): the grid/parameter types every phase operates on, plus
// the handful of functions reachable from more than one TU. Everything else
// stays internal (an anonymous namespace) to the TU that defines it -- see
// each .cpp's own comments for what that is. Export deliberately: this file
// grows only when a symbol gains an actual caller in another TU.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

constexpr double kSecondsPerYear = 31557600.0;

// ---------------------------------------------------------------- parameters

// TEST SEAM. `Descend` appends one entry per particle step when a probe is
// attached, so an invariant can watch a single particle's trajectory instead of
// inferring it from the terrain afterwards. Null on every production path, and
// the only thing the particle loop does with it is push_back.
//
// It exists because the velocity invariants (terminal velocity, swept path) are
// statements about the PARTICLE, and the dumped rasters cannot express them.
struct ParticleProbe {
  std::vector<float> speed;     // |velocity|; model units today, m/s after the
                                // dynamics rewrite
  std::vector<float> depth_m;   // flow depth the drag term used; 0 while no
                                // such concept exists
  std::vector<float> slope;     // local downslope gradient (rise/run)
  std::vector<int32_t> cell;    // cell index occupied at each step
};

struct Params {
  uint32_t seed = 1;
  int res = 1024;
  float world_m = 16384.0f;
  // Height unit 1.0 == this many metres. The dimensionless field does NOT have
  // to span [0,1]; `scale` and `max_diff` both derive from this consistently.
  // 400 rather than the old 900 because the target landform is rocky hills, and
  // 900 m over 16 km is a 5% mean gradient -- an alpine range, which is why the
  // audit measured 17 cm deep torrents.
  float relief_m = 400.0f;
  float noise_wavelength_m = 4096.0f;
  int noise_octaves = 8;

  // --- horseshoe substrate ---
  //
  // Three edges lifted into a rim, the fourth (y = res-1) the OUTFLOW edge
  // where all drainage leaves. Every term is multiplied by `u`, the normalised
  // distance inland, so the rim vanishes at the outflow and the surface falls
  // monotonically toward it. That monotonicity is load-bearing: an interior
  // minimum makes priority-flood flood the entire map as one basin, which is
  // the failure InitBowl's comment documents.
  //
  //   u        = (res-1 - y) / (res-1)
  //   d_rim    = min(x, res-1-x, y) / (res/2), clipped to [0,1]
  //   regional = tilt*u + rim*(1-d_rim)^p*u - trough*gauss(x)*u
  //   amp(u)   = amp_low + (amp_high - amp_low)*u^q
  //   z_m      = regional + amp(u)*fbm
  //
  // Defaults give ~300 m relief, ~1.6 deg median slope and ~61% of land under
  // 2 deg, against 12.3% on the audit's eroded fBm map.
  float tilt_m = 120.0f;       // ramp from the outflow edge to the far rim
  float rim_m = 150.0f;        // extra lift on the three rim edges
  float rim_exponent = 2.0f;   // NOT exposed: 1/2/4 measured indistinguishable
  float amp_high_m = 150.0f;   // fBm amplitude at the rim
  float amp_low_m = 25.0f;     // fBm amplitude at the outflow lowland
  float amp_taper_q = 1.5f;    // how fast amplitude falls toward the outflow
  // Optional master valley. OFF by default: at 150 m+ it cuts a trench the
  // outflow drowns rather than a valley a river runs down, and above ~250 m it
  // breaks the monotonicity the whole shape depends on.
  float trough_m = 0.0f;
  float trough_sigma_frac = 0.18f;
  // Synthetic fixture: a dish rising to `bowl_rim_m` at the edge with a gaussian
  // well at the centre, so everything drains inward to one closed basin. All
  // particles spawn at ONE point, which makes the flow path deterministic and
  // therefore gives "where should the sediment land" a right answer.
  // Synthetic terrains for the sanity tests. Each isolates ONE mechanism; the
  // toggles below let a test switch the others off so a failure has one cause.
  enum class Terrain {
    Noise, Bowl, Flat, Plane, Valley, Cliff, Lobe, Ridge, Horseshoe
  };
  // Horseshoe by default: bare fBm drains off all four edges, so flow disperses
  // instead of combining and the main stem gathers only 31% of the map. `Noise`
  // is kept, reachable with --fbm, so an A/B against the audit baseline in
  // docs/2026-08-03-stage1-erosion-benchmark.md stays possible.
  Terrain terrain = Terrain::Horseshoe;
  bool enable_erosion = true;
  bool enable_cascade = true;
  // Hillslope diffusion (soil creep). Off until the mechanism exists; the
  // invariant that describes it is already written and reports PEND.
  bool enable_diffusion = false;
  // Toggle for a mechanism that had none: without it a test silently included
  // the particle pass, and a change to particle stepping could move a
  // downstream result with nothing in the test to reveal it.
  bool enable_momentum = true;
  // Soil creep coefficient. Measured values sit at 1e-3..1e-2 m2/yr, which is
  // also what a ~10 My relaxation of a ~300 m hillslope implies (t ~ L^2/D) --
  // the two agree, which is the check that this is physics and not a fudge.
  // It cannot be honestly calibrated until the dynamics rewrite supplies a real
  // time axis; until then it is an unanchored constant and is off by default.
  float diffusion_D_m2_per_yr = 0.0f;
  // Attached only by tests. See ParticleProbe.
  ParticleProbe* probe = nullptr;
  bool bowl = false;  // legacy alias for terrain == Bowl
  float bowl_rim_m = 200.0f;
  float bowl_well_m = 100.0f;
  float bowl_sigma_frac = 0.12f;
  float source_x_frac = 0.5f, source_y_frac = 0.12f;
  float source_jitter_cells = 1.0f;

  int steps = 3000;
  int drops = 4096;
  // Water carried by one particle. SHOULD be a pure discretization choice:
  // halving `drops` while doubling this must give the same landscape, because
  // the two only ever appear as the product (total water). It does not hold
  // today -- `volume` leaks into gravity, momentum coupling, and erosion
  // magnitude -- and the invariant that says so is written and reports PEND.
  float drop_volume = 1.0f;

  // --- reference erosion constants (SimpleHydrology source). DO NOT RETUNE ---
  float min_vol = 0.01f;
  // Safety guard only, NOT a physics parameter any more. Lifetime is
  // max_travel_m; this just stops a degenerate trajectory spinning forever.
  int max_age = 20000;

  // --- transport, per unit DISTANCE rather than per iteration ---
  //
  // The reference closes a fixed fraction of the gap to equilibrium ONCE PER
  // LOOP ITERATION, so the erosion budget is set by how many times the loop
  // ran -- a numerical choice, not a physical one. That dependence is real and
  // was measured twice: changing the step from sqrt(2) cells to 1 cell rewrote
  // the landscape, and T2 drifted 1.649 -> 2.427 -> 3.176 m as parcels were
  // refined at IDENTICAL total water, never converging because there was no
  // continuum limit to converge to.
  //
  // Recast as an ADAPTATION LENGTH -- the distance over which flow reaches its
  // equilibrium concentration:  d(sediment)/ds = (c_eq - sediment) / L.
  // The same quantity the deleted in-lake plume used (L = u*h/w_s). Two
  // independent routes agree on the scale, which is the check that it is
  // physical and not a refit:
  //   back-calibrated from the old law:  1-exp(-22.6/L) = 0.1  ->  L ~ 215 m
  //   physical, silt-grade suspended load: u*h/w_s = 1*0.3/1e-3 -> ~300 m
  // Grain-size dependent: fine sand (w_s = 1e-2) would give 30 m instead.
  float adaptation_length_m = 215.0f;
  // Capacity scale, and INDEPENDENT of the adaptation length above -- the old
  // law entangled the two as (step length, 0.1 per step), which is why the
  // first attempt at this diverged. c_eq = (1 + e*d) * gradient * this, so it
  // converts a slope into the concentration the flow holds at equilibrium; the
  // adaptation length only says how fast it gets there.
  //
  // Back-calibrated to the old behaviour: erosion per unit distance is
  // (1+e*d) * grad * capacity/L, and matching the reference's 0.1 per
  // sqrt(2)-cell step needs capacity/L = 0.105, i.e. capacity ~ 22.6 m at
  // L = 215 m. Setting them equal cancels the 0.1 and erodes ~10x the local
  // drop per cell, which is a runaway.
  float capacity_length_m = 22.6f;
  // Evaporation as an e-folding TRAVEL distance. 22.6 km reproduces the old
  // 0.001 per sqrt(2)-cell step.
  float evap_length_m = 22600.0f;
  // Particle lifetime as distance travelled, replacing max_age's iteration
  // count: 500 steps x sqrt(2) cells x 16 m = 11.3 km.
  float max_travel_m = 11300.0f;
  float entrainment = 10.0f;
  float momentum_transfer = 1.0f;
  // Manning roughness, matching src/mapgen/erosion.hpp's ErosionParams so the
  // sim and the river graph share one roughness rather than disagreeing.
  float manning_n = 0.035f;
  // Overland sheet-flow depth on unchannelled ground. Real sheet flow is
  // millimetres; the value's job is to keep the drag term finite where there is
  // no channel, since c_f goes as h^(-1/3).
  float sheet_flow_depth_m = 0.005f;
  float lrate = 0.1f;  // the reference's value (World::lrate, world.h:42)
  float erf_scale = 0.4f;

  // --- two-layer substrate ---
  //
  // The surface is bedrock + soil. THE TRANSPORT LAW IS UNCHANGED: c_eq and the
  // deposition_rate step below are the reference's, verbatim. What the substrate
  // changes is only how much of the requested cut is actually YIELDED -- soil
  // gives way at full rate, bedrock at `bedrock_erodibility` of it. Deposition
  // is never resisted and always lands on soil.
  //
  // The ratio is k_bedrock/k_sediment from src/mapgen/erosion.hpp (5e-4 / 5e-3),
  // so the two generators agree on how much harder rock is.
  // --- soil production (bedrock weathering) ---
  // DECLARED, NOT YET IMPLEMENTED: P1-P4 are written against these and are red
  // until the mechanism lands.
  bool enable_soil_production = false;
  float soil_production_m_per_yr = 1e-4f;  // P0, bare-rock rate
  float soil_efold_m = 0.5f;               // h*, cover thickness that slows it
  float soil_strip_m_per_yr = 0.0f;        // synthetic constant removal, for P4
  float bedrock_erodibility = 0.1f;
  // Uniform starting soil, matching mapgen's initial_sediment_m. Applied as a
  // layer WITHIN the initial surface (bedrock = height - soil) rather than on
  // top of it, so the run starts from exactly the same terrain as before and an
  // A/B against a no-substrate run stays meaningful.
  float initial_soil_m = 4.0f;

  // Repose angle, in DEGREES -- the physical form of the reference's
  // dimensionless max_diff. 40 deg is steep rock; wet sediment is nearer 30.
  float repose_angle_deg = 40.0f;
  // Reference value. Safe again because the particle pass is serial and
  // in-place: the cascade is self-limiting there (the first application removes
  // the excess). It was only unsafe as a deferred/global pass.
  float settling = 0.8f;

  // --- climate, real units ---
  // Temperate default. evaporation_m_per_yr is temporarily UNCONSUMED: the
  // standing-water balance that read it against runoff was deleted with the
  // rest of the lake machinery. Retained for the Eulerian shallow-water phase
  // that replaces it; its KnobLiveness row is dropped until that phase gives
  // it an effect to prove.
  float runoff_m_per_yr = 1.0f;
  float evaporation_m_per_yr = 0.8f;

  // Regime width w = k_w*sqrt(Q), reused from the repo's erosion.hpp -- sets
  // the width term in the Manning flow-depth closure Descend uses off-channel
  // (see the depth_m derivation there).
  float channel_width_coeff = 5.0f;

  // THE landscape clock. One step represents this many years, and every
  // process scales with it: diffusion through k = D*dt/cell^2, and erosion
  // through the water a step delivers (see EffectiveDropVolume). Previously
  // erosion was step-based and diffusion was year-based, so a run could not
  // state how long it represented and D had no calibration to hang on.
  float dt_years = 200.0f;

  std::string out = "proto_out";
  int snapshot_every = 250;
};

struct Grid {
  int n = 0;
  size_t cells = 0;
  std::vector<float> height, height_b;        // DIMENSIONLESS ~[0,1]
  // Three discharge fields, and the distinction matters -- conflating them is
  // what unbounded the momentum force (see the audit).
  //   vol_ema   RAW EMA of accumulated particle volume. The reference's
  //             `cell.discharge`. Unbounded, and the ONLY correct denominator
  //             for the momentum force: it is what makes that force saturate.
  //   discharge erf(erf_scale * vol_ema), squashed ON READ as the reference's
  //             accessor does. Feeds c_eq's entrainment term only.
  //   Qm3s      physical discharge, m^3/s. Drives output and the flow depth
  //             the drag closure needs.
  std::vector<float> vol_ema, vol_ema_b;
  std::vector<float> discharge, discharge_b;
  std::vector<float> Qm3s, Qm3s_b;
  std::vector<float> momx, momy, momx_b, momy_b;
  // Erodible thickness over bedrock, height units. The surface is `height`;
  // bedrock is implicitly height - soil, so `height` stays authoritative and
  // every existing read of it is untouched.
  std::vector<float> soil;
  std::vector<float> vol_track, mx_track, my_track;
  std::vector<uint32_t> visits;
  // Mass balance, in height units x cells (see the residual print).
  double deposited_death = 0.0, lost_offmap = 0.0;

  explicit Grid(int res)
      : n(res), cells(size_t(res) * res), height(cells, 0.f),
        height_b(cells, 0.f),
        vol_ema(cells, 0.f), vol_ema_b(cells, 0.f),
        discharge(cells, 0.f), discharge_b(cells, 0.f),
        Qm3s(cells, 0.f), Qm3s_b(cells, 0.f),
        momx(cells, 0.f), momy(cells, 0.f), momx_b(cells, 0.f), momy_b(cells, 0.f),
        soil(cells, 0.f),
        vol_track(cells, 0.f), mx_track(cells, 0.f), my_track(cells, 0.f),
        visits(cells, 0u) {}

  inline size_t idx(int x, int y) const { return size_t(y) * n + x; }
  inline bool oob(int x, int y) const {
    return x < 1 || y < 1 || x >= n - 1 || y >= n - 1;
  }
};

// Extracted from main so the sanity tests exercise the REAL loop rather than
// a copy that can drift from it.
struct SimStats {
  double t_drops = 0, t_grid = 0;
};

// Definitions in protogen.cpp -- phase-0 land init, the whole-sim driver, and
// the repose-angle cascade the test suite exercises directly (CascadeThresholdGate).
void InitTerrain(Grid& g, const Params& p);
void RunSim(const Params& p, Grid& g, SimStats& st, bool verbose);
void Cascade(Grid& g, const Params& p, int x, int y, float max_diff);

}  // namespace pg

// The test entry point. Defined in protogen_tests.cpp; called from main().
namespace test {
int RunAll();
}
