// THROWAWAY PROTOTYPE -- particle hydraulic erosion (SimpleHydrology / Nick
// McDonald, nickmcd.me 2023-12-12) with standing water, lake water balance and
// plume deposition.
//
// EROSION IS THE REFERENCE'S, UNCHANGED. An earlier revision swapped it for
// stream power (E = K*A^m*S^n) on the theory that the reference's
// `c_eq = (1 + entrainment*discharge)*dh` was "the same law re-parameterised".
// That was wrong: one is transport-limited, the other detachment-limited, they
// behave nothing alike, and the substitution produced blurry featureless
// terrain where the reference produces dendritic dissection. The law below is
// verbatim from the reference and is not to be "improved" again.
//
// Height is DIMENSIONLESS in ~[0,1] internally, as the reference works, so its
// tuned constants keep their meaning. Physical units are confined to REPORTING
// (metres on dump) and to the lake water balance, which needs real m^3/yr
// against real m/yr and cannot be expressed any other way.
//
// What is new relative to the reference:
//   * lakes: priority-flood depressions + evaporation/inflow water balance
//   * sediment DISPERSES in standing water instead of being dumped at a point
//   * parallel over particles via the repo's Taskflow pool, deterministically
//
// build (Taskflow is header-only, so this stays a standalone TU -- no CMake):
//   c++ -O3 -std=c++20 \
//     -I<repo>/src -I<repo>/third_party/FastNoiseLite \
//     -I<repo>/build/_deps/taskflow-src \
//     protogen.cpp <repo>/src/core/parallel.cpp -o protogen

#include <algorithm>
#include <array>
#include <numeric>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "FastNoiseLite.h"
#include "core/parallel.hpp"

// The OUTPUT BOUNDARY: world.txt's manifest and rivers.bin's river-graph
// dump, plus the river extraction pipeline that fills the latter. None of
// this touches the sim above -- it reads the FINISHED height/water/soil
// fields, the same fields the .f32 dump already carries.
#include "mapgen/coarse_io.hpp"
#include "mapgen/river_graph.hpp"
#include "mapgen/river_io.hpp"
#include "mapgen/river_prune.hpp"

namespace {

constexpr double kSecondsPerYear = 31557600.0;
// Real gravity. The motion law integrates a genuine velocity in m/s now, so
// this is an acceleration, not the reference's dimensionless force scale.
constexpr float kGravityMS2 = 9.81f;
// The dt_years at which the transport constants were calibrated. A landscape
// step represents dt_years, and BOTH processes must scale with it or they are
// on different clocks -- which is what stopped the diffusion coefficient from
// being calibratable at all.
constexpr float kCalibrationYears = 200.0f;

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
  bool enable_lake_deposit = true;
  // Hillslope diffusion (soil creep). Off until the mechanism exists; the
  // invariant that describes it is already written and reports PEND.
  bool enable_diffusion = false;
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
  bool disperse = true;  // --no-disperse dumps at the entry cell instead
  // Fill every basin to its spill level on the first build. For the A/B
  // test only: it removes the fill-up transient so a short run has a lake.
  bool prefill = false;

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
  // The same quantity the in-lake plume already uses (L = u*h/w_s). Two
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
  // Integration step in METRES, so it does not inherit the grid. A step may
  // cross several cells; the swept segment is traversed and the law applied per
  // cell by path length, so this sets integration accuracy only and refining it
  // must not change the landscape (T2).
  float travel_step_m = 16.0f;
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

  // --- climate, real units (lake water balance only) ---
  // Temperate default: evaporation BELOW runoff, so every basin fills to spill
  // and overflows and evaporation culls nothing -- culling is the explicit
  // min-area/min-depth prune. Evaporation only culls at R/E ~ 0.01-0.05.
  float runoff_m_per_yr = 1.0f;
  float evaporation_m_per_yr = 0.8f;

  // --- sediment dispersion in standing water ---
  // Grain settling velocity sets how far a plume reaches before it has dropped
  // its load: fine sand 1e-2, silt 1e-3, clay 1e-6 m/s.
  float settling_velocity_m_per_s = 1.0e-2f;
  // Velocity of the plume INSIDE the lake, for the settling length
  // L = u*h/w_s. This is not the river's inlet speed: a jet decelerates to a
  // few cm/s within a handful of channel widths. Feeding it 1 m/s gave
  // L = 1*260/0.01 = 26 km against a 500 m lake, so nothing settled inside and
  // every particle carried its load to the outlet and dumped it there -- which
  // is why the delta came out thickest at the FAR shore.
  float plume_velocity_m_per_s = 0.08f;
  // How many in-lake steps the plume deposits over before it is CUT OFF.
  //
  // Beyond a few steps the load is essentially spent (4 steps at L = 37 m and
  // 16 m cells sheds 1 - exp(-64/37) = 82% of it), and continuing to shed all
  // the way to the spill point is what put the thickest deposit at the OUTLET
  // instead of the inlet: every particle steers at the same outlet cell, so
  // those cells collect a shed from every single visitor. After the cutoff the
  // particle still travels to the outlet and exits -- it just lays nothing
  // down -- so the outflow river below the lake keeps its discharge.
  // As a DISTANCE, not a step count -- 4 steps x 16 m under the old fixed
  // stride. Everything else in the transport law is per-metre now, and leaving
  // this as an iteration count made it depend on the integration step.
  float lake_deposit_length_m = 64.0f;
  // Lateral plume wander: std dev, in DEGREES, of the per-step angular
  // perturbation applied to the outlet-ward heading inside a lake.
  //
  // This is the missing lateral term. The along-path decay
  // qs*(1-exp(-cell/L)) spreads the load DOWNSTREAM correctly, but deposits it
  // into the one cell the particle occupies -- so with a straight-line traverse
  // every delta is a 1-D stripe. Perturbing the heading makes the traverse a
  // random walk biased toward the outlet, so lateral offset accumulates as
  // sqrt(distance): turbulent dispersion, which is what the dropped
  // grad.(K grad C) term describes. 0 reproduces the old straight line.
  // Spread half-angle of the plume, constant. 40 deg maximised lake coverage
  // in the sweep; 0 gives a straight line, which is the A/B control.
  float plume_wander_deg = 40.0f;
  // NOTE: the spread angle is deliberately CONSTANT. A velocity-dependent
  // version (atan(u_turb/v) with a decaying jet) was built and worked, but the
  // particle has no real speed to drive it -- the reference normalises speed to
  // sqrt(2) every step -- so it had to be fed a made-up entry velocity. Keeping
  // it constant avoids dressing a constant up as physics.
  // Regime width w = k_w*sqrt(Q), reused from the repo's erosion.hpp. Sets the
  // jet's inlet width, which is the length scale its decay is measured in.
  float channel_width_coeff = 5.0f;

  // --- lakes ---
  // Water is accumulated EVERY step (see UpdateLakes); this only sets how often
  // the basin topology -- spill level and hypsometry -- is re-derived.
  //
  // 25 because that is inside the CONVERGED regime. Between rebuilds the
  // hypsometry is stale, so a basin filling with sediment still reports its old
  // capacity and the lake looks alive. Measured at 300 steps: intervals 5, 10
  // and 25 all agree the lake has silted up (0% wet) while 50 and 100 report it
  // still there (2.66% / 2.25%). The old default of 50 sat in the lagging
  // regime, so lakes were being kept alive by a numerical artefact.
  int lake_interval = 25;
  // THE landscape clock. One step represents this many years, and every
  // process scales with it: diffusion through k = D*dt/cell^2, and erosion
  // through the water a step delivers (see EffectiveDropVolume). Previously
  // erosion was step-based and diffusion was year-based, so a run could not
  // state how long it represented and D had no calibration to hang on.
  float dt_years = 200.0f;
  // A cell must hold at least this much water before the plume/dispersion path
  // engages. Transient sheet flow is not a lake and must not trigger it.
  float min_dispersion_depth_m = 1.0f;
  // Water leaves the map at or below this. For the horseshoe it coincides with
  // the outflow edge, which the generator already places at 0.
  float sea_level_m = 0.0f;
  // Ordered sweeps of the water relaxation per step, warm-started. Cold start
  // at t = 0 runs to convergence instead.
  int water_sweeps = 4;
  // Total loss from standing water, amortised over dt_years: evaporation plus
  // seepage plus unresolved sub-grid drainage. It exceeds pan evaporation for
  // the same reason an effective roughness exceeds a grain diameter -- the
  // model resolves less than reality and the parameter absorbs the rest.
  //
  // It is also the lake-size dial, exactly: equilibrium is an area balance,
  //   loss * lake_area = runoff * catchment_area
  // so lake_area/catchment_area = runoff/loss. A basin overflows iff its area
  // at spill is below that ratio of its own catchment -- inflow scales with
  // catchment while basin area does not, so trunk basins always spill and only
  // a large lake on a small catchment becomes a sink. Which is what an
  // endorheic lake is.
  float standing_water_loss_m_per_yr = 0.0f;  // UNUSED: evaporation_m_per_yr is the one
  // The water sub-system runs on its OWN clock: source and sink both dwarf
  // storage at landscape pace (1 m/yr over 200 yr is 200 m of rain against
  // ~10 m lakes), and water equilibrates in years while terrain takes 1e4-1e6.
  float dt_water_yr = 0.01f;
  int water_iters = 12;     // per landscape step, warm-started
  int water_prime_iters = 3000;  // once at t = 0, from dry
  float min_lake_area_m2 = 1.0e4f;  // 1 ha
  float min_lake_depth_m = 0.5f;

  std::string out = "proto_out";
  int snapshot_every = 250;
};

// -------------------------------------------------------------------- fields

// Water carried by one particle, scaled so that drops x volume is the water a
// LANDSCAPE STEP delivers. Erosion scales with volume, so this is what puts
// erosion on the same clock as diffusion. T2 proves splitting the same water
// into a different number of parcels does not change the landscape, so this is
// purely a statement about how much water a step represents -- not a knob.
inline float EffectiveDropVolume(const Params& p) {
  return p.drop_volume * (p.dt_years / kCalibrationYears);
}

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
  //   Qm3s      physical discharge, m^3/s. Drives lakes, output, and now the
  //             flow depth the drag closure needs.
  std::vector<float> vol_ema, vol_ema_b;
  std::vector<float> discharge, discharge_b;
  std::vector<float> Qm3s, Qm3s_b;
  std::vector<float> momx, momy, momx_b, momy_b;
  std::vector<float> water;                   // standing depth, height units
  // Relaxed water SURFACE (height units), the Planchon-Darboux fixed point.
  // Persisted across steps so the relaxation can warm-start.
  std::vector<float> wsurf;
  // Erodible thickness over bedrock, height units. The surface is `height`;
  // bedrock is implicitly height - soil, so `height` stays authoritative and
  // every existing read of it is untouched.
  std::vector<float> soil;
  std::vector<int32_t> lake_id;
  std::vector<int32_t> lake_outlet;
  // kept lake id -> index into the Lake vector. lake_id stores the KEPT id
  // (pruned lakes are skipped), so indexing `lakes` with it directly is
  // off by however many were pruned before it.
  std::vector<int32_t> lake_index;
  std::vector<float> vol_track, mx_track, my_track;
  std::vector<uint32_t> visits;
  // Mass balance, in height units x cells (see the residual print).
  double deposited_lake = 0.0, deposited_death = 0.0, lost_offmap = 0.0;

  explicit Grid(int res)
      : n(res), cells(size_t(res) * res), height(cells, 0.f),
        height_b(cells, 0.f),
        vol_ema(cells, 0.f), vol_ema_b(cells, 0.f),
        discharge(cells, 0.f), discharge_b(cells, 0.f),
        Qm3s(cells, 0.f), Qm3s_b(cells, 0.f),
        momx(cells, 0.f), momy(cells, 0.f), momx_b(cells, 0.f), momy_b(cells, 0.f),
        water(cells, 0.f), wsurf(cells, 0.f), soil(cells, 0.f),
        lake_id(cells, -1),
        vol_track(cells, 0.f), mx_track(cells, 0.f), my_track(cells, 0.f),
        visits(cells, 0u) {}

  inline size_t idx(int x, int y) const { return size_t(y) * n + x; }
  inline bool oob(int x, int y) const {
    return x < 1 || y < 1 || x >= n - 1 || y >= n - 1;
  }
};

// --- substrate ---------------------------------------------------------------
//
// Every mutation of the surface goes through these two, so `height == bedrock +
// soil` cannot drift and soil can never go negative.

// Lays `amount` of loose material down. Deposition is never resisted -- what
// settles is soil by definition, whatever it was eroded from.
inline void Deposit(Grid& g, size_t c, float amount) {
  if (amount <= 0.f) return;
  g.soil[c] += amount;
  g.height[c] += amount;
}

// Cuts `depth` from the surface and returns what the substrate ACTUALLY yielded.
// Soil goes first at full rate; once it is gone the cut bites bedrock, which
// gives up only `bedrock_erodibility` of what was asked. A bare-rock cell
// therefore lowers ~10x slower than an alluvial one under identical forcing --
// which is what makes ridges hold up and valleys fill.
inline float Erode(Grid& g, const Params& p, size_t c, float depth) {
  if (depth <= 0.f) return 0.f;
  const float from_soil = std::min(depth, g.soil[c]);
  const float from_rock = (depth - from_soil) * p.bedrock_erodibility;
  g.soil[c] -= from_soil;
  const float removed = from_soil + from_rock;
  g.height[c] -= removed;
  return removed;
}

struct V2 { float x = 0, y = 0; };
inline float len(V2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline V2 unit(V2 a) {
  const float l = len(a);
  return l > 0.f ? V2{a.x / l, a.y / l} : V2{0.f, 0.f};
}

// -------------------------------------------------------------- initial land

// A RAMP with a PIT in it: height falls linearly from the source edge (y=0) to
// the far edge (y=world), with a gaussian well at the centre.
//
// It must drain off the far edge. An earlier version used a dish rising
// outward, which makes the map centre the global minimum -- priority flood then
// floods the ENTIRE map as one basin whose shore is the map border, where
// discharge is zero, so it never fills and no lake ever appears.
void InitBowl(Grid& g, const Params& p) {
  const float cell = p.world_m / float(p.res);
  const float cx = 0.5f * p.world_m, cy = 0.5f * p.world_m;
  const float sigma = p.bowl_sigma_frac * p.world_m;
  for (int y = 0; y < g.n; ++y)
    for (int x = 0; x < g.n; ++x) {
      const float wx = x * cell - cx, wy = y * cell - cy;
      const float r2 = wx * wx + wy * wy;
      const float ramp = p.bowl_rim_m * (1.0f - (y * cell) / p.world_m);
      const float well = p.bowl_well_m * std::exp(-r2 / (2.0f * sigma * sigma));
      g.height[g.idx(x, y)] = (ramp - well) / p.relief_m;
    }
}

// Analytic fixtures. All are expressed in METRES then divided by relief_m, so
// a test can state its expectation in metres.
void InitAnalytic(Grid& g, const Params& p) {
  const float cell = p.world_m / float(p.res);
  const float cx = 0.5f * p.world_m, cy = 0.5f * p.world_m;
  const float sigma = p.bowl_sigma_frac * p.world_m;
  for (int y = 0; y < g.n; ++y)
    for (int x = 0; x < g.n; ++x) {
      const float wx = x * cell - cx, wy = y * cell - cy;
      float h = 0.f;
      switch (p.terrain) {
        case Params::Terrain::Flat:
          h = 0.5f * p.bowl_rim_m;  // dead level: erosion must do nothing
          break;
        case Params::Terrain::Plane:
          // Uniform slope down +y. Discharge at the foot must equal
          // runoff x upstream area.
          h = p.bowl_rim_m * (1.0f - (y * cell) / p.world_m);
          break;
        case Params::Terrain::Valley:
          // Slope down +y plus a V in x: flow must converge on the centre col.
          h = p.bowl_rim_m * (1.0f - (y * cell) / p.world_m) +
              p.bowl_well_m * std::fabs(wx) / (0.5f * p.world_m);
          break;
        case Params::Terrain::Cliff:
          // A vertical step ON A SLOPE. Without the tilt both plateaus are dead
          // flat, particles have no gradient, they die where they land and the
          // step is never visited -- the test then measures nothing.
          h = 0.25f * p.bowl_rim_m * (1.0f - (y * cell) / p.world_m) +
              ((y * cell < cy) ? p.bowl_rim_m : 0.0f);
          break;
        case Params::Terrain::Ridge:
          // A 1-D gaussian ridge running along y, on a DEAD FLAT base. Only x
          // varies, so diffusion acts in one dimension and the analytic answer
          // (peak falls, flanks rise, mass conserved) has no second axis to
          // confound it. Deliberately not tilted: a slope would let particles
          // and the cascade move material and the test would stop isolating
          // diffusion.
          h = p.bowl_rim_m * std::exp(-(wx * wx) / (2.0f * sigma * sigma));
          break;
        case Params::Terrain::Lobe: {
          // Gaussian hill, centre offset by HALF A CELL. Centred exactly on a
          // cell it is perfectly symmetric, so central differences give an
          // exactly zero gradient there and no particle can ever move off it.
          const float ox = wx - 0.5f * cell, oy = wy - 0.5f * cell;
          h = p.bowl_rim_m *
              std::exp(-(ox * ox + oy * oy) / (2.0f * sigma * sigma));
          break;
        }
        default:
          h = 0.f;
          break;
      }
      g.height[g.idx(x, y)] = h / p.relief_m;
    }
}

// Seeds the soil layer. Applied as a layer WITHIN the initial surface -- the
// bedrock is implicitly height - soil -- so `height` is bit-identical to a
// no-substrate run at step 0 and the A/B stays honest. A cell only starts
// behaving differently once it has eroded through its starting soil.
void InitSoil(Grid& g, const Params& p) {
  const float s = (p.relief_m > 0.f) ? p.initial_soil_m / p.relief_m : 0.f;
  std::fill(g.soil.begin(), g.soil.end(), s);
}

// Normalised fBm in [-0.5, 0.5], shared by the fBm and horseshoe terrains so
// they see the identical noise field and an A/B isolates the SHAPE.
void FbmCentred(const Params& p, int n, std::vector<float>& out) {
  FastNoiseLite noise(int(p.seed));
  noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  noise.SetFractalType(FastNoiseLite::FractalType_FBm);
  noise.SetFractalOctaves(p.noise_octaves);
  noise.SetFrequency(1.0f / p.noise_wavelength_m);
  const float cell = p.world_m / float(p.res);
  out.assign(size_t(n) * n, 0.f);
  float lo = 1e30f, hi = -1e30f;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float v = noise.GetNoise(x * cell, y * cell);
      out[size_t(y) * n + x] = v;
      lo = std::min(lo, v); hi = std::max(hi, v);
    }
  const float inv = (hi > lo) ? 1.0f / (hi - lo) : 1.0f;
  for (float& v : out) v = (v - lo) * inv - 0.5f;
}

// The horseshoe: a rim on three edges opening onto one outflow edge, with the
// noise amplitude tapering from high inland to subdued in the lowland. See the
// Params block for the closed form and why every term carries `u`.
void InitHorseshoe(Grid& g, const Params& p) {
  const int n = g.n;
  std::vector<float> fbm;
  FbmCentred(p, n, fbm);
  const float cxf = 0.5f * float(n - 1);
  const float half = 0.5f * float(n);
  const float sigma = std::max(p.trough_sigma_frac * float(n), 1e-3f);
  for (int y = 0; y < n; ++y) {
    // 0 at the outflow edge (y = n-1), 1 at the far rim.
    const float u = float(n - 1 - y) / float(n - 1);
    for (int x = 0; x < n; ++x) {
      const float d_rim = std::clamp(
          float(std::min(std::min(x, n - 1 - x), y)) / half, 0.f, 1.f);
      float regional = p.tilt_m * u +
                       p.rim_m * std::pow(1.0f - d_rim, p.rim_exponent) * u;
      if (p.trough_m > 0.f) {
        const float dx = float(x) - cxf;
        regional -= p.trough_m * std::exp(-(dx * dx) / (2.0f * sigma * sigma)) * u;
      }
      const float amp = p.amp_low_m + (p.amp_high_m - p.amp_low_m) *
                                          std::pow(u, p.amp_taper_q);
      g.height[g.idx(x, y)] =
          (regional + amp * fbm[size_t(y) * n + x]) / p.relief_m;
    }
  }
}

void InitTerrain(Grid& g, const Params& p) {
  InitSoil(g, p);
  if (p.bowl || p.terrain == Params::Terrain::Bowl) { InitBowl(g, p); return; }
  if (p.terrain == Params::Terrain::Horseshoe) { InitHorseshoe(g, p); return; }
  if (p.terrain != Params::Terrain::Noise) { InitAnalytic(g, p); return; }
  FastNoiseLite n(int(p.seed));
  n.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  n.SetFractalType(FastNoiseLite::FractalType_FBm);
  n.SetFractalOctaves(p.noise_octaves);
  n.SetFrequency(1.0f / p.noise_wavelength_m);
  const float cell = p.world_m / float(p.res);
  float lo = 1e30f, hi = -1e30f;
  for (int y = 0; y < g.n; ++y)
    for (int x = 0; x < g.n; ++x) {
      const float v = n.GetNoise(x * cell, y * cell);
      g.height[g.idx(x, y)] = v;
      lo = std::min(lo, v); hi = std::max(hi, v);
    }
  const float inv = (hi > lo) ? 1.0f / (hi - lo) : 1.0f;
  for (float& h : g.height) h = (h - lo) * inv;
}

// ------------------------------------------------------------ priority flood

// Barnes et al. Every cell raised to the lowest elevation on any path to the
// map edge -- which is its spill level.
void PriorityFlood(const Grid& g, std::vector<float>& filled,
                   std::vector<int32_t>& outlet_of) {
  const int n = g.n;
  filled.assign(g.height.begin(), g.height.end());
  outlet_of.assign(g.cells, -1);
  std::vector<uint8_t> closed(g.cells, 0);
  struct Item {
    float e; int32_t cell, src;
    bool operator>(const Item& o) const { return e > o.e; }
  };
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  auto seed = [&](int x, int y) {
    const size_t i = g.idx(x, y);
    if (closed[i]) return;
    closed[i] = 1;
    pq.push({g.height[i], int32_t(i), int32_t(i)});
  };
  for (int x = 0; x < n; ++x) { seed(x, 0); seed(x, n - 1); }
  for (int y = 0; y < n; ++y) { seed(0, y); seed(n - 1, y); }

  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  while (!pq.empty()) {
    const Item it = pq.top(); pq.pop();
    const int x = it.cell % n, y = it.cell / n;
    for (int k = 0; k < 4; ++k) {
      const int nx = x + dx[k], ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
      const size_t j = g.idx(nx, ny);
      if (closed[j]) continue;
      closed[j] = 1;
      const bool flooded = g.height[j] < it.e;
      filled[j] = flooded ? it.e : g.height[j];
      outlet_of[j] = flooded ? it.src : int32_t(j);
      pq.push({filled[j], int32_t(j), outlet_of[j]});
    }
  }
}

// ------------------------------------------------------- water relaxation

// Water surface by ORDERED-SWEEP relaxation (Planchon-Darboux).
//
// The filled surface is the fixed point of
//     w[i] = max( h[i], min over 4-neighbours j of w[j] )
// with w pinned to h wherever water leaves the map. That is the SAME answer
// PriorityFlood computes -- but as a bounded, warm-startable grid pass instead
// of a global priority queue. Which is what lets it run every step (no rebuild
// cadence to be wrong about) and what makes it portable to a compute shader:
// a priority queue is the one part of this sim that cannot go on a GPU.
//
// MUST be initialised from ABOVE. The iteration has other fixed points
// reachable from below: on h = [10,5,0,5,10] with pinned ends, starting at
// w = h sticks at [10,5,5,5,10] rather than filling to 10. From above, w
// decreases monotonically to the correct surface, so a partial sweep budget
// leaves lakes slightly TOO FULL -- a conservative error that shrinks with more
// sweeps, rather than a wrong answer.
//
// Sweeps alternate scan direction. A Jacobi pass propagates information one
// cell per iteration, so a long spillway would need O(n) of them; an ordered
// sweep carries it the whole length of the scan line in one pass, which is why
// priming is affordable.
void RelaxWater(Grid& g, const Params& p, int sweeps, bool cold) {
  const int n = g.n;
  const float inf = 1e30f;
  const float sea_hu_r = p.sea_level_m / p.relief_m;
  auto is_sea_r = [&](int x, int y) {
    return g.height[g.idx(x, y)] < sea_hu_r;
  };
  auto pinned = [&](int x, int y) {
    return is_sea_r(x, y) || x == 0 || y == 0 || x == n - 1 || y == n - 1;
  };
  // Held value differs by KIND: the sea holds its surface AT sea level; an
  // outflow edge holds it at the terrain, i.e. dry.
  auto held = [&](int x, int y) {
    return std::max(g.height[g.idx(x, y)], sea_hu_r);  // same one expression
  };
  if (cold) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t i = g.idx(x, y);
        g.wsurf[i] = pinned(x, y) ? held(x, y) : inf;
      }
  } else {
    // Warm start. Valid while the previous surface sits at or above the new
    // solution, which incision guarantees. If a DIVIDE aggrades the spill can
    // rise above the old surface and this starts from below, under-filling that
    // basin until a cold pass; lifting to h is the cheap half of the repair.
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t i = g.idx(x, y);
        g.wsurf[i] = pinned(x, y) ? held(x, y)
                                  : std::max(g.wsurf[i], g.height[i]);
      }
  }
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  for (int s = 0; s < sweeps; ++s) {
    const bool rev_x = (s & 1) != 0, rev_y = (s & 2) != 0;
    for (int yy = 0; yy < n; ++yy) {
      const int y = rev_y ? n - 1 - yy : yy;
      for (int xx = 0; xx < n; ++xx) {
        const int x = rev_x ? n - 1 - xx : xx;
        if (pinned(x, y)) continue;
        const size_t i = g.idx(x, y);
        float lo = inf;
        for (int k = 0; k < 4; ++k) {
          const int ax = x + dx4[k], ay = y + dy4[k];
          if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;
          lo = std::min(lo, g.wsurf[g.idx(ax, ay)]);
        }
        // In place, so the rest of this scan line sees the update -- that is
        // the whole speed advantage over a Jacobi pass.
        const float want = std::max(g.height[i], lo);
        if (want < g.wsurf[i]) g.wsurf[i] = want;
      }
    }
  }
}

// --------------------------------------------------------- per-cell water

// Standing water as a FIELD, with no lake objects at all.
//
// Per cell: rain in, loss out where wet, and a conservative levelling transport
// that moves water toward lower surfaces. Lakes are then simply cells with
// depth > 0, and their extent is an equilibrium rather than something computed:
// a basin grows until loss over its wet cells consumes its inflow, or until it
// reaches its spill and the surplus runs off. One rule covers both regimes, and
// the arid case is the same code with a different ratio.
//
// Replaces BuildLakes' hypsometry and UpdateLakes' per-lake budget: no
// sorted_beds, no prefix sums, no LevelFromVolume, no shoreline inflow
// accounting, no lake_interval.
//
// WHY A SUB-TIMESTEP. Source and sink both dwarf storage at landscape pace --
// runoff 1 m/yr over dt_years = 200 puts 200 m of water on every cell in one
// step, against lakes ~10 m deep. Water equilibrates in years, terrain over
// 1e4-1e6 years, so the water sub-system is integrated on its own clock and
// warm-started. That quasi-steady split is the load-bearing assumption here.
void UpdateWater(Grid& g, const Params& p, int iters,
                 std::vector<float>& flux) {
  const int n = g.n;
  const float cell_m = p.world_m / float(n);
  const float sea_hu = p.sea_level_m / p.relief_m;
  // Rain and loss for one water sub-step, in height units.
  const float loss = p.evaporation_m_per_yr * p.dt_water_yr / p.relief_m;
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  // TWO OPPOSITE boundary conditions, and collapsing them into one predicate is
  // what inverted the sea. Order matters: sea level wins over the edge, because
  // an edge cell below sea level is ocean, not a drain.
  //
  //   below sea level -> SEA.     Surface held AT sea level, so depth is
  //                               sea - height. It can only ADD water.
  //   map edge, above -> OUTFLOW. Water leaves, depth 0.
  //
  // The old code applied the outflow rule to BOTH, so sub-sea ground was
  // emptied every iteration instead of filled (T13/T14).
  auto is_boundary = [&](int x, int y) {
    return x == 0 || y == 0 || x == n - 1 || y == n - 1 ||
           g.height[g.idx(x, y)] < sea_hu;
  };
  // ONE expression, not two branches: a boundary cell holds water up to sea
  // level and drains only the EXCESS above it.
  //     depth = max(0, sea_level - height)
  // Below sea level that fills (it is ocean); above it that is zero (the excess
  // runs off). The earlier code forced depth to 0 unconditionally, which
  // emptied sub-sea ground instead of filling it -- T13/T14.
  auto held_depth = [&](size_t i) {
    return std::max(0.f, sea_hu - g.height[i]);
  };

  for (int it = 0; it < iters; ++it) {
    // --- inflow where flow is TRAPPED, loss from STANDING water ------------
    //
    // Two different processes on two different timescales, and putting them in
    // one loop is what killed the water.
    //
    // Water running downhill crosses a catchment in hours; a lake equilibrates
    // over years. Transit water is ALREADY transported -- that is the discharge
    // field the particles build -- and it has no residence time in the cells it
    // passes through, so it cannot evaporate at the standing rate. Adding rain
    // everywhere and then taxing it made every parcel die in transit: loss was
    // 0.2 m per iteration against 0.01 m of rain, and nothing ever arrived.
    //
    // So the field only handles water that gets TRAPPED: a cell where the water
    // surface has no downhill neighbour is a bottom, and the discharge arriving
    // there stops being transit and starts being storage. Everything else is
    // handled by the particles.
    const double dt_s = double(p.dt_water_yr) * kSecondsPerYear;
    const double cell_area = double(cell_m) * double(cell_m);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t i = g.idx(x, y);
        if (is_boundary(x, y)) { g.water[i] = held_depth(i); continue; }
        const float wi = g.height[i] + g.water[i];
        bool trapped = true;
        for (int k = 0; k < 4; ++k) {
          const int ax = x + dx4[k], ay = y + dy4[k];
          if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;
          if (g.height[g.idx(ax, ay)] + g.water[g.idx(ax, ay)] < wi) {
            trapped = false;
            break;
          }
        }
        if (trapped) {
          // Depth the arriving discharge adds over this sub-step.
          g.water[i] += float(double(g.Qm3s[i]) * dt_s / cell_area /
                              double(p.relief_m));
        }
        // Loss applies to STANDING water only -- which, now that transit is not
        // stored here, is all this field contains.
        if (g.water[i] > 0.f) g.water[i] = std::max(0.f, g.water[i] - loss);
      }

    // --- volume-conserving levelling, by ORDERED SWEEP ---------------------
    //
    // In place and in scan order, so water cascades the length of the scan line
    // in ONE pass. A Jacobi pass instead moves a quarter of the head per
    // iteration, which is diffusion: it converges in O(N^2), measured at 26.8 m
    // of residual over-fill after 4000 iterations on 64^2, and 1024^2 would
    // need ~1e6. Same machinery as RelaxWater's sweeps, different operator --
    // this one conserves the water it has, where Planchon-Darboux fills to
    // spill regardless of how much exists and so cannot level a finite volume.
    //
    // Convergence is NOT required. Partial levelling leaves a surface that is
    // merely not yet flat; the accumulation it carries is still the physical
    // one, which is the property worth having.
    (void)flux;
    for (int s = 0; s < 4; ++s) {
      const bool rev_x = (s & 1) != 0, rev_y = (s & 2) != 0;
      for (int yy = 0; yy < n; ++yy) {
        const int y = rev_y ? n - 1 - yy : yy;
        for (int xx = 0; xx < n; ++xx) {
          const int x = rev_x ? n - 1 - xx : xx;
          const size_t i = g.idx(x, y);
          if (g.water[i] <= 0.f || is_boundary(x, y)) continue;
          // Push to the LOWEST neighbouring surface only. Spreading over all
          // four is what makes it diffusive; steepest descent is advective, so
          // a column runs downhill rather than smearing outward.
          int bk = -1;
          float best = g.height[i] + g.water[i];
          for (int k = 0; k < 4; ++k) {
            const int ax = x + dx4[k], ay = y + dy4[k];
            if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;
            const size_t j = g.idx(ax, ay);
            const float wj = g.height[j] + g.water[j];
            if (wj < best) { best = wj; bk = k; }
          }
          if (bk < 0) continue;  // local minimum of the water surface: it ponds
          const size_t j = g.idx(x + dx4[bk], y + dy4[bk]);
          // Half the head difference equalises the pair without overshooting
          // into oscillation; capped by what the cell actually holds.
          const float head = (g.height[i] + g.water[i]) - best;
          const float t = std::min(g.water[i], 0.5f * head);
          g.water[i] -= t;
          // Boundary cells are re-clamped to held_depth each iteration, so what
          // arrives there is not stored -- it has left the map or joined the sea.
          if (!is_boundary(x + dx4[bk], y + dy4[bk])) g.water[j] += t;
        }
      }
    }
  }
}

// ---------------------------------------------------------------- lake build

// Per-lake state, cached between the periodic topology rebuilds so the water
// budget can be integrated EVERY step rather than only at a rebuild.
// A connected body of standing water. Geometry ONLY -- the water FIELD says how
// much water there is, so the hypsometry (sorted_beds/prefix, a per-lake sort),
// volume_m3, level, and the shoreline inflow accounting are all gone with the
// budget they served.
struct Lake {
  std::vector<uint32_t> members;
  int32_t outlet = -1;
};

// Connected bodies of standing water, with an exit for each. Replaces
// BuildLakes: the water field already decided how much water there is and
// where, so there is no priority flood, no hypsometry, no sort, and no rebuild
// cadence to be stale.
void LabelWater(Grid& g, const Params& p, std::vector<Lake>& lakes,
                int& n_lakes, float& wet_frac, float& deepest_m) {
  const int n = g.n;
  const float cell_area = (p.world_m / float(n)) * (p.world_m / float(n));
  lakes.clear();
  std::fill(g.lake_id.begin(), g.lake_id.end(), -1);
  g.lake_outlet.clear(); g.lake_index.clear();
  static const int dx4[4]={1,-1,0,0}, dy4[4]={0,0,1,-1};
  std::vector<uint32_t> stack;
  size_t wet=0; float deepest=0.f;
  for (int y0=0;y0<n;++y0) for (int x0=0;x0<n;++x0) {
    const size_t seed=g.idx(x0,y0);
    if (g.water[seed]<=0.f || g.lake_id[seed]>=0) continue;
    const int32_t id=int32_t(lakes.size());
    Lake lk; stack.assign(1,uint32_t(seed)); g.lake_id[seed]=id;
    int32_t exit_cell=-1; float exit_surf=1e30f;
    while(!stack.empty()){
      const uint32_t c=stack.back(); stack.pop_back(); lk.members.push_back(c);
      const int cx=int(c)%n, cy=int(c)/n;
      for(int k=0;k<4;++k){
        const int ax=cx+dx4[k], ay=cy+dy4[k];
        if(ax<0||ay<0||ax>=n||ay>=n) continue;
        const size_t j=g.idx(ax,ay);
        if(g.water[j]>0.f){ if(g.lake_id[j]<0){g.lake_id[j]=id; stack.push_back(uint32_t(j));} }
        else if(g.height[j]<exit_surf){ exit_surf=g.height[j]; exit_cell=int32_t(j); }
      }
    }
    lk.outlet = exit_cell>=0?exit_cell:int32_t(seed);
    wet+=lk.members.size();
    for(uint32_t c:lk.members) deepest=std::max(deepest,g.water[c]);
    lakes.push_back(std::move(lk));
  }
  g.lake_outlet.resize(lakes.size()); g.lake_index.resize(lakes.size());
  n_lakes=0;
  for(size_t i=0;i<lakes.size();++i){
    g.lake_outlet[i]=lakes[i].outlet; g.lake_index[i]=int32_t(i);
    float d=0.f; for(uint32_t c:lakes[i].members) d=std::max(d,g.water[c]);
    if(float(lakes[i].members.size())*cell_area>=p.min_lake_area_m2 &&
       d*p.relief_m>=p.min_lake_depth_m) ++n_lakes;
  }
  wet_frac=float(double(wet)/double(g.cells));
  deepest_m=deepest*p.relief_m;
}






// Bilinear height at a CONTINUOUS position, cell centres at (i+0.5, j+0.5).
//
// The transport law needs the drop along the path divided by the distance that
// spans -- a directional derivative. Taking it from cell indices instead
// returns the full cell-to-cell drop however short the step was, so a 4 m step
// over 16 m cells reads a gradient 4x too steep and the landscape diverges
// under refinement. Interpolating makes the sample distance and the drop agree
// at any step size, and it still contains h[here], which is what keeps the law
// self-limiting.
inline float SampleHeight(const Grid& g, float fx, float fy) {
  const float px = fx - 0.5f, py = fy - 0.5f;
  int x0 = int(std::floor(px)), y0 = int(std::floor(py));
  const float tx = px - float(x0), ty = py - float(y0);
  x0 = std::clamp(x0, 0, g.n - 2);
  y0 = std::clamp(y0, 0, g.n - 2);
  const float h00 = g.height[g.idx(x0, y0)], h10 = g.height[g.idx(x0 + 1, y0)];
  const float h01 = g.height[g.idx(x0, y0 + 1)],
              h11 = g.height[g.idx(x0 + 1, y0 + 1)];
  return (h00 * (1.f - tx) + h10 * tx) * (1.f - ty) +
         (h01 * (1.f - tx) + h11 * tx) * ty;
}

void Cascade(Grid& g, const Params& p, int x, int y, float max_diff);

// Amanatides-Woo voxel traversal over the swept segment, in CELL coordinates.
// Calls body(cx, cy, ds_cells) for every cell the segment passes through, with
// the path length inside that cell, in order.
//
// This is what lets a step cross several cells without breaking the physics:
// the transport law is applied per cell weighted by how far the particle
// actually travelled through it, so displacement no longer has to be clamped to
// one cell. Deterministic quadrature over the path -- picking a cell at random
// would have the same expectation but add variance and an RNG draw, and would
// cost the bit-identical A/B this codebase relies on.
template <typename F>
void TraverseSegment(V2 a, V2 b, F&& body) {
  int x = int(std::floor(a.x)), y = int(std::floor(a.y));
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float seg = std::sqrt(dx * dx + dy * dy);
  if (!(seg > 0.f)) { body(x, y, 0.f); return; }
  const int sx = dx > 0.f ? 1 : (dx < 0.f ? -1 : 0);
  const int sy = dy > 0.f ? 1 : (dy < 0.f ? -1 : 0);
  const float inf = 1e30f;
  float tmx = (sx > 0) ? (float(x + 1) - a.x) / dx
                       : (sx < 0 ? (float(x) - a.x) / dx : inf);
  float tmy = (sy > 0) ? (float(y + 1) - a.y) / dy
                       : (sy < 0 ? (float(y) - a.y) / dy : inf);
  const float tdx = (sx != 0) ? std::fabs(1.0f / dx) : inf;
  const float tdy = (sy != 0) ? std::fabs(1.0f / dy) : inf;
  float t = 0.f;
  // Bounded: a segment is at most a few cells, and the guard stops a degenerate
  // direction from spinning.
  for (int guard = 0; guard < 256; ++guard) {
    const float t_next = std::min(std::min(tmx, tmy), 1.0f);
    body(x, y, (t_next - t) * seg);
    t = t_next;
    if (t >= 1.0f) return;
    if (tmx < tmy) { x += sx; tmx += tdx; } else { y += sy; tmy += tdy; }
  }
}

// ------------------------------------------------------------------- descend

// The reference's descend, verbatim, plus a lake branch. Serial and IN PLACE:
// particles interact through the terrain within a step and that coupling is
// part of the physics (see README).
void Descend(Grid& g, const Params& p, std::vector<Lake>& lakes,
             std::mt19937& rng, float px, float py) {
  const float cell_m = p.world_m / float(p.res);
  const float scale = p.relief_m / cell_m;  // height units per cell width
  // Repose threshold in height units: tan(angle) * cell / relief.
  const float max_diff = std::tan(p.repose_angle_deg * 3.14159265f / 180.0f) *
                         cell_m / p.relief_m;
  V2 pos{px, py}, speed{0.f, 0.f};
  float volume = EffectiveDropVolume(p), sediment = 0.0f;
  // `sediment` is a CONCENTRATION, not a mass: it is rescaled by /(1-evap_rate)
  // in step with `volume *= (1-evap_rate)`, so the two only conserve as a
  // PRODUCT. The transport step already respects this -- the terrain moves
  // `volume * deposition_rate * cdiff` while the particle's concentration moves
  // `deposition_rate * cdiff`.
  //
  // Every EXIT path must convert too. Booking the bare concentration deposits
  // 1/volume too much (1.65x by max_age, which is the dominant exit) and
  // overstates the off-map loss by the same factor. That was a 4.59% mass leak
  // at production length, invisible to a 200-step fixture. One helper so a
  // future exit path cannot quietly reintroduce it.
  auto carried_mass = [&]() -> float { return sediment * volume; };
  float lake_travel_m = 0.f;  // distance covered in standing water, for the
                             // deposition cutoff
  float total_travel_m = 0.f;  // lifetime is a DISTANCE now, not an iteration count
  size_t last_cell = g.idx(int(px), int(py));

  for (int age = 0; age < p.max_age; ++age) {
    const int x = int(pos.x), y = int(pos.y);
    if (g.oob(x, y)) { g.lost_offmap += double(carried_mass()); return; }
    const size_t here = g.idx(x, y);
    last_cell = here;
    ++g.visits[here];

    if (volume < p.min_vol) {
      Deposit(g, here, carried_mass());
      g.deposited_death += double(carried_mass());
      return;
    }

    // --- standing water -------------------------------------------------
    // THRESHOLD: only water deep enough to be a lake engages the plume path.
    // Transient sheet flow over a damp cell is not standing water and must keep
    // behaving as a river, or dispersion fires on every wet-ish channel cell.
    if (g.water[here] * p.relief_m >= p.min_dispersion_depth_m) {
      const int32_t lid = g.lake_id[here];
      if (lid < 0 || lid >= int32_t(g.lake_outlet.size())) {
        Deposit(g, here, carried_mass());
        g.deposited_death += double(carried_mass());
        return;
      }
      const int32_t target = g.lake_outlet[lid];

      lake_travel_m += cell_m;
      const bool depositing =
          p.enable_lake_deposit && lake_travel_m <= p.lake_deposit_length_m;
      float drop;
      if (!depositing) {
        drop = 0.f;
      } else if (p.disperse) {
        // A laden inflow is a hypopycnal plume: velocity collapses at the
        // mouth, the load settles over L = u*h/w_s, and the steady state of
        // advection-diffusion-settling thins exponentially. The load lost
        // crossing ONE cell is therefore qs*(1 - exp(-cell/L)) -- the flux
        // DIVERGENCE. Depositing the flux itself is dimensionally the wrong
        // quantity and diverged to 2e5 m of relief.
        const float depth_m = g.water[here] * p.relief_m;
        // No upper clamp. The old 64-cell cap was left over from when this
        // was an O(R^2) kernel splat; deposition is O(1) along the path now, so
        // there is no cost reason for it -- and it silently killed the grain
        // knob, collapsing everything finer than ~1e-2 m/s to the same reach.
        // A 100 km L for clay is physically right: it means "does not settle
        // inside this lake", which is what lake-floor clay does.
        const float L_m = std::max(
            p.plume_velocity_m_per_s * depth_m /
                std::max(p.settling_velocity_m_per_s, 1e-9f),
            cell_m);
        drop = sediment * (1.0f - std::exp(-cell_m / L_m));
      } else {
        drop = sediment;  // control: dump the whole load at the entry cell
      }
      drop = std::min(drop, sediment);
      sediment -= drop;
      // `drop` is a share of the CONCENTRATION; what lands on the bed is the
      // corresponding mass.
      const float drop_mass = drop * volume;
      Deposit(g, here, drop_mass);
      g.deposited_lake += double(drop_mass);
      // Sediment displacing water needs no bookkeeping: the bed rises and the
      // water field re-levels over it next pass.
      if (p.enable_cascade) Cascade(g, p, x, y, max_diff);

      // Cross to the spill point so discharge continues downstream instead of
      // being swallowed. Discharge is NOT accumulated here: a lake surface is
      // not a channel, and stamping it inflates the field.
      const float tx = float(int(target) % g.n) + 0.5f;
      const float ty = float(int(target) / g.n) + 0.5f;
      V2 dir = unit(V2{tx - pos.x, ty - pos.y});
      if (len(dir) <= 0.f) {
        // Standing exactly ON the spill cell. The particle must NOT stop here:
        // it still carries whatever the plume cutoff left it, and dumping that
        // at the outlet is exactly the "thickest deposit at the OUTLET" defect
        // the README records -- every visitor steers to the same cell, so they
        // all shed onto it. Water leaving a lake keeps going, so carry on in
        // the direction of travel and let the next iteration treat it as river
        // or lake on its merits.
        dir = unit(speed);
        if (len(dir) <= 0.f) {
          // Genuinely nowhere to go: no heading and no momentum. Then it does
          // stop, and a particle that stops sheds its load.
          Deposit(g, here, carried_mass());
          g.deposited_death += double(carried_mass());
          return;
        }
      }
      // Wander only while the plume is live; once spent it is just water
      // heading for the spill point.
      if (p.plume_wander_deg > 0.f && depositing) {
        // Mean heading is the outlet, so the walk is WEIGHTED toward it; the
        // gaussian angular jitter is what spreads the plume sideways. Lateral
        // offset then accumulates as sqrt(distance) -- turbulent dispersion.
        std::normal_distribution<float> jitter(
            0.f, p.plume_wander_deg * 3.14159265f / 180.f);
        const float a = std::atan2(dir.y, dir.x) + jitter(rng);
        dir = V2{std::cos(a), std::sin(a)};
      }
      pos = V2{pos.x + dir.x, pos.y + dir.y};
      // Evaporate exactly as the land path does (see the bottom of this loop):
      // evaporation removes WATER, so the suspended concentration rises to
      // match and sediment*volume is unchanged.
      //
      // Decaying volume alone -- which this branch did -- destroys evap_rate of
      // the particle's carried MASS on every in-lake step. That is small per
      // step and ruinous in aggregate: a particle random-walks across a lake at
      // one cell per step, so a wide basin costs hundreds of steps, and it is
      // the 0.774% production mass deficit that survived the
      // concentration-as-mass fix. It barely registers on the steep synthetic
      // fixtures because they pond almost nothing, which is why 200-step tests
      // on a cliff could never have caught it.
      const float ev_lake = std::exp(-cell_m / p.evap_length_m);
      sediment /= ev_lake;
      volume *= ev_lake;
      total_travel_m += cell_m;
      if (total_travel_m >= p.max_travel_m) break;
      continue;
    }

    // --- reference erosion, unchanged ------------------------------------
    const float dhx =
        (g.height[g.idx(x + 1, y)] - g.height[g.idx(x - 1, y)]) * 0.5f;
    const float dhy =
        (g.height[g.idx(x, y + 1)] - g.height[g.idx(x, y - 1)]) * 0.5f;
    // gx,gy are -dz/dx,-dz/dy (metres per metre) and nl = sqrt(1+|grad z|^2),
    // so (gx,gy)/nl is exactly sin(theta) down the line of steepest descent.
    const float gx = -scale * dhx, gy = -scale * dhy;
    const float nl = std::sqrt(gx * gx + gy * gy + 1.0f);
    const float slope = std::sqrt(gx * gx + gy * gy);  // rise/run

    // Flow depth, from the LOCAL DISCHARGE via the same regime + Manning
    // closure river_graph.cpp uses -- so the particle dynamics and the river
    // graph finally agree instead of contradicting each other.
    //   w = k_w*sqrt(Q),  d = (Q*n/(w*sqrt(S)))^0.6  =>  d = (n/k_w)^0.6 *
    //                                                     Q^0.3 * S^-0.3
    // Floored at a sheet-flow depth: overland flow on an unchannelled hillslope
    // is millimetres deep, not zero, and a zero depth sends the drag term to
    // infinity and stalls every headwater particle.
    const float S_h = std::max(slope, 1e-4f);  // river_graph's kMinChannelSlope
    float depth_m = p.sheet_flow_depth_m;
    if (g.Qm3s[here] > 0.f)
      depth_m = std::max(depth_m,
                         std::pow(p.manning_n / p.channel_width_coeff, 0.6f) *
                             std::pow(g.Qm3s[here], 0.3f) *
                             std::pow(S_h, -0.3f));

    // Gravity: mass-independent, as it must be. The old form divided by volume,
    // which made a bigger parcel accelerate LESS -- unphysical, and load-bearing
    // only because the sqrt(2) renormalisation below discarded the magnitude
    // anyway.
    const V2 a_grav{kGravityMS2 * gx / nl, kGravityMS2 * gy / nl};
    // Drag, Manning closure c_f = g*n^2/h^(1/3), so a_drag = -B*|u|*u with
    // B = g*n^2/h^(4/3). Balancing it against gravity gives terminal velocity
    // u = h^(2/3)*sqrt(S)/n -- Manning's equation falls straight out, which is
    // the check that this is the same physics as the river graph.
    const float B = kGravityMS2 * p.manning_n * p.manning_n /
                    std::pow(depth_m, 4.0f / 3.0f);
    const float u_term = std::pow(depth_m, 2.0f / 3.0f) *
                         std::sqrt(S_h) / p.manning_n;

    // ONE TAP = ONE CELL LENGTH, physics re-derived every tap.
    //
    // NOT aligned to cell boundaries. Running to the next boundary made every
    // particle change direction ON A GRID LINE -- a grid synchronisation, and
    // the likely source of the vertical striping, since the sweep scans rows.
    // A fixed step of one cell length has no such alignment: where the tap ends
    // depends on where the particle started, not on the lattice.
    const float sp0 = len(speed);
    const V2 hdg = (sp0 > 0.f) ? V2{speed.x / sp0, speed.y / sp0}
                               : V2{gx / std::max(slope, 1e-9f),
                                    gy / std::max(slope, 1e-9f)};
    if (std::fabs(hdg.x) < 1e-6f && std::fabs(hdg.y) < 1e-6f) {
      Deposit(g, here, carried_mass());
      g.deposited_death += double(carried_mass());
      return;
    }
    const float cross_cells = 1.0f;
    const float ds_step_m = cross_cells * cell_m;
    const float dt = ds_step_m / std::max(std::max(sp0, u_term), 0.01f);

    // IMPLICIT in the drag term, solved exactly rather than linearised.
    //
    // Explicit Euler diverges whenever dt exceeds the drag relaxation time,
    // which it routinely does (~10 s relaxation against a ~16 s step). But
    // damping with the OLD speed is no better: from rest sp0 = 0 gives no
    // damping at all, and the particle takes a full undamped gravity step --
    // measured 26.5 m/s against a 0.118 m/s terminal velocity.
    //
    // So solve v = (v0 + a*dt)/(1 + B*v*dt) for v, which is a quadratic:
    //   B*dt*v^2 + v - (v0 + a*dt) = 0
    //   v = (-1 + sqrt(1 + 4*B*dt*v_free)) / (2*B*dt)
    // Unconditionally stable, exact at steady state (v -> sqrt(a/B), i.e.
    // Manning), and correct to first order when drag is weak.
    const V2 free{speed.x + a_grav.x * dt, speed.y + a_grav.y * dt};
    const float v_free = len(free);
    if (v_free > 0.f) {
      const float bd = B * dt;
      const float v = (bd > 1e-12f)
                          ? (std::sqrt(1.0f + 4.0f * bd * v_free) - 1.0f) /
                                (2.0f * bd)
                          : v_free;
      speed = V2{free.x / v_free * v, free.y / v_free * v};
    } else {
      speed = V2{0.f, 0.f};
    }

    // Momentum transfer. Denominator is the RAW volume EMA, as the reference
    // has it (water.h:99 uses cell->discharge, NOT the erf'd accessor). Since
    // |f| itself grows with traffic, that raw term is what makes this force
    // SATURATE near |speed| instead of growing without bound; using the
    // squashed field capped the denominator at 2 and let the force reach ~4900,
    // 30-3000x gravity, which welded channels in place.
    const V2 f{g.momx[here], g.momy[here]};
    if (len(f) > 0.f && len(speed) > 0.f) {
      const float k = p.momentum_transfer * dot(unit(f), unit(speed)) /
                      (volume + g.vol_ema[here]);
      speed = V2{speed.x + k * f.x, speed.y + k * f.y};
    }
    if (len(speed) <= 0.f) {
      // Gravity and the momentum field cancelled, or the cell is dead flat with
      // no stream to follow. Same rule as every other exit: what the particle
      // is carrying must land somewhere. Returning bare destroyed it, which is
      // the residual that survived the concentration-vs-mass fix -- it scales
      // with how much flat ground a map has, so it barely shows on the steep
      // synthetic fixtures and shows clearly at production scale.
      Deposit(g, here, carried_mass());
      g.deposited_death += double(carried_mass());
      return;
    }
    // No sqrt(2) renormalisation. Speed is a real velocity in m/s now, bounded
    // by drag rather than by fiat, and the displacement below is u*dt.

    if (p.probe) {
      p.probe->speed.push_back(len(speed));
      p.probe->depth_m.push_back(depth_m);
      p.probe->slope.push_back(slope);
      p.probe->cell.push_back(int32_t(here));
    }

    // Exactly one cell per iteration, so there is no multi-cell segment to walk.
    const V2 pos_next{pos.x + hdg.x * cross_cells, pos.y + hdg.y * cross_cells};
    bool left_map = false;
    float travelled_m = 0.f;
    {
      const int cx = x, cy = y;
      const size_t ci = here;
      const float ds_m = ds_step_m;
      travelled_m += ds_m;

      // Discharge accumulates PER UNIT DISTANCE, normalised so that crossing a
      // whole cell contributes `volume` exactly once however many integration
      // steps that took. Adding `volume` per traversed cell instead makes a
      // 4 m step count a 16 m cell four times, inflating discharge as the step
      // shrinks -- and discharge drives c_eq, so the landscape diverged under
      // refinement (measured 1.47 / 2.40 / 9.96 m at 16 / 8 / 4 m).
      const float share = ds_m / cell_m;
      g.vol_track[ci] += volume * share;
      g.mx_track[ci] += volume * speed.x * share;
      g.my_track[ci] += volume * speed.y * share;
      ++g.visits[ci];

      // Capacity re-derived HERE, for THIS cell -- not carried in from the
      // start of the step. A segment spanning several cells crosses terrain the
      // start cell knows nothing about, so one shared c_eq averages over a
      // ridge it never saw; that is what made multi-cell steps unsafe.
      // Replaying the update per cell removes the limit: measured stable across
      // a 32x range of step sizes, where before it diverged.
      //
      // The gradient is taken one cell along the flow FROM THIS CELL, so it is
      // step-independent and still contains this cell's own height -- eroding
      // it shrinks its own capacity, the negative feedback the scheme rests on.
      // Sampled one cell along the ACTUAL flow direction, interpolated -- NOT
      // snapped to the nearest of 8 neighbours.
      //
      // The snap evaluated the gradient along a LATTICE direction rather than
      // the flow, so its error aligned with the grid and erosion carved
      // straight diagonals radiating from the corners. Confirmed generated
      // here, not inherited: the initial terrain has none. T11 measures what is
      // left rather than leaving it to the eye, which is how the snap was found.
      //
      // The near end stays the DISCRETE cell height. Interpolating both ends
      // makes sensing continuous while erosion stays per-cell, which
      // under-damps the feedback (7.5e5 m). Dropping the forward sample
      // entirely for grad(h).heading -- the form with no sampling at all -- is
      // the honest one and was tried twice: it ran to 1e11 m before the
      // gradient limiter existed, and with the limiter it flattens the terrain
      // until particles stall and burn max_age at 98% CPU. Recorded so the
      // third attempt starts from the failure mode, not from the idea.
      const float ahead_h = SampleHeight(g, float(cx) + 0.5f + hdg.x,
                                         float(cy) + 0.5f + hdg.y);
      const float dist_m = cell_m;
      const float drop_hu = g.height[ci] - ahead_h;
      float c_eq = (1.0f + p.entrainment * g.discharge[ci]) *
                   (drop_hu / dist_m) * p.capacity_length_m;
      if (c_eq < 0.f) c_eq = 0.f;

      // Relaxation over the distance actually travelled inside THIS cell.
      // d(sediment)/ds = (c_eq - sediment)/L integrates to this exactly, so
      // splitting a step in two gives the same total -- which is the whole
      // point.
      const float f = 1.0f - std::exp(-ds_m / p.adaptation_length_m);
      const float cdiff = p.enable_erosion ? (c_eq - sediment) : 0.f;
      float want = volume * cdiff * f;
      // GRADIENT LIMITER. A traversal may not excavate more than the drop it is
      // descending: removing exactly the drop flattens the cell, removing more
      // INVERTS it, and the next particle then sees a bigger drop and cuts
      // deeper still. That is a runaway, and it is why the scheme was stable
      // only for volume <= 1 -- erosion scales with volume, so a fat parcel
      // overshot on its own.
      //
      // This is a stability bound of the same kind as CFL, not a tuning knob:
      // it binds only when a step would reverse the local topography, which is
      // exactly where the discretization has stopped being meaningful. Below
      // that it changes nothing.
      if (want > 0.f) {
        const float drop_share =
            std::max(0.f, drop_hu) * std::min(1.0f, ds_m / cell_m);
        want = std::min(want, drop_share);
      }
      float realized;
      if (want >= 0.f) {
        realized = Erode(g, p, ci, want);
      } else {
        Deposit(g, ci, -want);
        realized = want;  // deposition is never resisted
      }
      // The particle gains EXACTLY what the ground lost, derived from
      // `realized` rather than re-derived from `want`.
      //
      // The old form scaled cdiff*f by realized/want, which is only equal to
      // this while want == volume*cdiff*f. The gradient limiter breaks that
      // identity, and re-deriving from a clamped `want` silently leaked 6.6% of
      // the mass. Taking it from `realized` is exact through the substrate
      // yield AND the limiter, and cannot drift if another clamp is added.
      sediment += realized / volume;

      if (p.enable_cascade) Cascade(g, p, cx, cy, max_diff);
    }
    if (g.oob(int(pos_next.x), int(pos_next.y))) left_map = true;
    pos = pos_next;
    if (left_map) { g.lost_offmap += double(carried_mass()); return; }

    // Evaporation per unit DISTANCE, for the same reason as the transport law:
    // a per-iteration factor would make particle lifetime depend on the step.
    // Water leaves, the suspended concentration rises, mass is unchanged.
    const float ev = std::exp(-travelled_m / p.evap_length_m);
    sediment /= ev;
    volume *= ev;

    // Lifetime is a travel distance, not an iteration count.
    total_travel_m += travelled_m;
    if (total_travel_m >= p.max_travel_m) break;
  }
  // Reached max_age still carrying a load. The reference deposits it here;
  // dropping that made this a silent mass sink, and it is the DOMINANT exit --
  // volume only decays to 0.61 over 500 steps, so the volume < min_vol branch
  // above is unreachable in practice.
  Deposit(g, last_cell, carried_mass());
  g.deposited_death += double(carried_mass());
}

// ------------------------------------------------------------------- cascade

// Thermal erosion / sediment avalanching at one cell, IN PLACE -- the
// reference's version, restored.
//
// This is an EROSIONAL TRANSPORT mechanism, not a smoothing filter: it is how
// material actually moves downslope out of the channels the particles cut.
// An earlier revision made it a gentle global pass (needed when particle writes
// were deferred for parallelism) and that cost 44 m of relief and most of the
// fine valley structure. The particle pass is serial and in-place again, so the
// reference form is safe: applying it here removes the excess immediately, so
// the next particle through sees corrected terrain and it cannot compound.
void Cascade(Grid& g, const Params& p, int x, int y, float max_diff) {
  if (g.oob(x, y)) return;
  static const int cdx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int cdy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
  struct Nb { size_t i; float d; };
  Nb nb[8];
  int count = 0;
  float steepest = 0.f;
  const size_t here = g.idx(x, y);
  for (int k = 0; k < 8; ++k) {
    const int ax = x + cdx[k], ay = y + cdy[k];
    if (g.oob(ax, ay)) continue;
    const size_t a = g.idx(ax, ay);
    const float d = g.height[here] - g.height[a];
    if (d > 0.f) { nb[count++] = {a, d}; steepest = std::max(steepest, d); }
  }
  if (count == 0) return;
  // Nothing is over the repose angle, so the settle loop below would take
  // `continue` on every neighbour and touch nothing. Leaving before the sort is
  // therefore BIT-IDENTICAL, and it is worth doing: the sort alone measured 17%
  // of total runtime, and most cells sit under repose most of the time (23.6%
  // of the map exceeded 40 deg at the end of the benchmark run, far less
  // earlier).
  if (steepest <= max_diff) return;
  // NOT filtered to `d > max_diff` at gather time, though that looks tempting
  // and would shrink the sort further. std::sort is UNSTABLE, so dropping
  // elements can permute EQUAL drops differently -- and equal drops are the
  // normal case on the synthetic fixtures (T5's neighbourhood has all eight
  // identical). The result would stop being bit-identical exactly where the
  // tests are most symmetric. The early-out above needs no such argument.
  //
  // Steepest drop settles first.
  std::sort(nb, nb + count, [](const Nb& a, const Nb& b) { return a.d > b.d; });
  for (int k = 0; k < count; ++k) {
    // Re-read: the previous iteration lowered height[here], so a snapshotted
    // drop is stale and the sort no longer bounds the remainder -- hence
    // `continue`, not `break`. Using stale drops could transfer more than the
    // excess and push this cell below the neighbours it was shedding onto.
    const float excess = (g.height[here] - g.height[nb[k].i]) - max_diff;
    if (excess <= 0.f) continue;
    const float transfer = p.settling * excess * 0.5f;
    // Talus is a transport of loose material, so it moves through the substrate
    // too: a bare-rock cell sheds only a tenth as fast, which is what keeps
    // rock slopes standing steeper than alluvial ones.
    Deposit(g, nb[k].i, Erode(g, p, here, transfer));
  }
}

// ----------------------------------------------------------------- diffusion

// Hillslope diffusion -- soil creep. dz/dt = D * laplacian(z).
//
// This is the mechanism the sim has been missing, and it is why erosion made
// terrain YOUNGER: the repose cascade fires only above 40 deg, so nothing at
// all acted on the ~76% of the map below it and ridges could never round.
// Creep is continuous on every slope. The two are different processes --
// landsliding is a threshold event, creep is not -- so this runs ALONGSIDE the
// cascade, never instead of it. (The README records that REPLACING the cascade
// with a global smoothing pass cost 44 m of relief and the fine valley
// structure; that is a result about replacement.)
//
// Conservative by construction: each edge contributes +k*(z_j - z_i) to i and
// +k*(z_i - z_j) to j, which sum to zero. Out-of-bounds neighbours are simply
// skipped, giving no-flux boundaries, so nothing leaves the map either.
//
// Two Jacobi sub-passes so it stays parallel AND respects the substrate:
// creep moves SOIL, so a cell may not shed more than it has. Sub-pass 1 finds
// each cell's outgoing demand and the fraction of it the soil can actually
// supply; sub-pass 2 applies the per-edge flux scaled by the DONOR's fraction,
// which both endpoints can compute identically. Scaling after the fact instead
// would break conservation.
void Diffuse(Grid& g, const Params& p, std::vector<float>& demand,
             std::vector<float>& limit, std::vector<float>& delta) {
  if (!p.enable_diffusion || p.diffusion_D_m2_per_yr <= 0.f) return;
  const float cell_m = p.world_m / float(p.res);
  // k = D*dt/cell^2, dimensionless. The explicit 4-neighbour Laplacian is
  // stable only for k <= 0.25; beyond that it oscillates and diverges rather
  // than smoothing, so clamp loudly rather than produce quiet garbage.
  float k = p.diffusion_D_m2_per_yr * p.dt_years / (cell_m * cell_m);
  if (k > 0.25f) k = 0.25f;
  const int n = g.n;
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};

  // Sub-pass 1: how much each cell wants to shed, and how much of that its soil
  // can cover. Bedrock is not immobile, it just yields slowly -- same ratio the
  // fluvial law uses -- so a stripped ridge creeps ~10x slower than a mantled one.
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      float out = 0.f;
      for (int m = 0; m < 4; ++m) {
        const int ax = x + dx4[m], ay = y + dy4[m];
        if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;
        const float d = g.height[i] - g.height[g.idx(ax, ay)];
        if (d > 0.f) out += k * d;
      }
      demand[i] = out;
      const float avail = g.soil[i] + (out > g.soil[i]
                                           ? (out - g.soil[i]) * p.bedrock_erodibility
                                           : 0.f);
      limit[i] = (out > 0.f) ? std::min(1.0f, avail / out) : 1.0f;
    }
  });

  // Sub-pass 2: per-edge flux, scaled by the DONOR's limit. Both endpoints see
  // the same donor and the same factor, so the exchange balances exactly.
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      float d_sum = 0.f;
      for (int m = 0; m < 4; ++m) {
        const int ax = x + dx4[m], ay = y + dy4[m];
        if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;
        const size_t j = g.idx(ax, ay);
        const float diff = g.height[i] - g.height[j];
        if (diff > 0.f) d_sum -= k * diff * limit[i];        // i is the donor
        else if (diff < 0.f) d_sum += k * (-diff) * limit[j];  // j is the donor
      }
      delta[i] = d_sum;
    }
  });

  // Apply. Soil tracks the surface: what creeps away leaves the mantle, what
  // arrives joins it, and soil may not go negative.
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const size_t base = yy * size_t(n);
    for (int x = 0; x < n; ++x) {
      const size_t i = base + x;
      g.height[i] += delta[i];
      g.soil[i] = std::max(0.f, g.soil[i] + delta[i]);
    }
  });
}

// -------------------------------------------------------------------- output

void Dump(const Grid& g, const Params& p, const std::string& tag) {
  std::vector<float> tmp(g.cells);
  auto write = [&](const char* name, const std::vector<float>& f, float sc) {
    for (size_t i = 0; i < g.cells; ++i) tmp[i] = f[i] * sc;
    const std::string path = p.out + "/" + tag + "-" + name + ".f32";
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
      std::fprintf(stderr, "protogen: cannot write %s\n", path.c_str());
      return;
    }
    std::fwrite(tmp.data(), sizeof(float), tmp.size(), fp);
    std::fclose(fp);
  };
  write("height", g.height, p.relief_m);   // metres
  write("water", g.water, p.relief_m);     // metres
  write("discharge", g.discharge, 1.0f);
  write("Q", g.Qm3s, 1.0f);                // m^3/s
  write("soil", g.soil, p.relief_m);       // metres of erodible cover
}

// Runs the whole simulation. Extracted from main so the sanity tests exercise
// the REAL loop rather than a copy that can drift from it.
struct SimStats {
  int n_lakes = 0;
  float wet_frac = 0.f, deepest_m = 0.f;
  double t_drops = 0, t_grid = 0, t_lake = 0;
};

void RunSim(const Params& p, Grid& g, std::vector<Lake>& lakes, SimStats& st,
            bool verbose) {
  const float world_area = p.world_m * p.world_m;
  // Divides by TOTAL water shed per step (drops x volume), not by the drop
  // count. The runoff a map sheds is a physical quantity and must not change
  // because the same water was chopped into a different number of parcels.
  // Identical at the default drop_volume of 1.0.
  // max(..., 1) guards the grid-pass-only fixtures, which run with drops = 0 to
  // isolate a grid mechanism from the particles.
  const double q_per_unit_vol_m3_s =
      double(p.runoff_m_per_yr) * double(world_area) /
      std::max(double(p.drops) * double(EffectiveDropVolume(p)), 1.0) /
      kSecondsPerYear;
  std::mt19937 rng(p.seed ^ 0x9e3779b9u);
  std::uniform_real_distribution<float> uni(1.0f, float(p.res - 2));
  std::vector<float> sx(p.drops), sy(p.drops);
  // Diffusion scratch, allocated once rather than per step.
  std::vector<float> diff_demand(g.cells), diff_limit(g.cells),
      diff_delta(g.cells);
  std::vector<float> water_flux(g.cells);
  int n_lakes = 0;
  float wet_frac = 0.f, deepest_m = 0.f;
  using clk = std::chrono::steady_clock;
  double t_drops = 0, t_grid = 0, t_lake = 0;
  auto secs = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
  };
  // Only the bowl wants one inlet (it is testing a delta). The other
  // fixtures want rain everywhere, or the hill never gets touched.
  const bool single_source =
      p.bowl || p.terrain == Params::Terrain::Bowl;

  for (int step = 1; step <= p.steps; ++step) {
    // Spawn points drawn on the main thread, so the particle set is a pure
    // function of the seed and independent of how work is chunked.
    if (single_source) {
      // ONE source. The flow path is then deterministic, so "where should the
      // sediment land" has a right answer rather than a distribution.
      const float ox = p.source_x_frac * float(p.res);
      const float oy = p.source_y_frac * float(p.res);
      std::uniform_real_distribution<float> jit(-p.source_jitter_cells,
                                                p.source_jitter_cells);
      for (int d = 0; d < p.drops; ++d) {
        sx[d] = std::clamp(ox + jit(rng), 1.0f, float(p.res - 2));
        sy[d] = std::clamp(oy + jit(rng), 1.0f, float(p.res - 2));
      }
    } else {
      for (int d = 0; d < p.drops; ++d) { sx[d] = uni(rng); sy[d] = uni(rng); }
    }

    // SERIAL and IN PLACE, as the reference is. Particles interact through the
    // terrain within a step -- the first to cross a steep cell lowers it, so the
    // next sees a smaller drop and erodes less. Deferring the writes into
    // per-worker buffers makes every particle in a step see the same
    // uncorrected terrain, so erosion compounds by the visit count and the
    // field runs to 1e36 (measured). The reference parallelises only because
    // its writes are racy AND in place; a deterministic merge cannot reproduce
    // that. Grid passes below are still parallel -- they are genuine Jacobi
    // updates and safe.
    auto tA = clk::now();
    std::fill(g.vol_track.begin(), g.vol_track.end(), 0.f);
    std::fill(g.mx_track.begin(), g.mx_track.end(), 0.f);
    std::fill(g.my_track.begin(), g.my_track.end(), 0.f);
    std::fill(g.visits.begin(), g.visits.end(), 0u);
    for (int d = 0; d < p.drops; ++d) Descend(g, p, lakes, rng, sx[d], sy[d]);

    auto tC = clk::now(); t_drops += secs(tA, tC);
    // EMA, double buffered: one writer per cell, so no atomics. Averaging is
    // what lets a channel persist between steps and attract later drops.
    const float lr = p.lrate, es = p.erf_scale;
    badlands::ParallelFor(size_t(p.res), [&](size_t yy) {
      const size_t base = yy * size_t(p.res);
      for (int x = 0; x < p.res; ++x) {
        const size_t i = base + x;
        // EMA over the RAW accumulation, then squash on read -- the reference's
        // order (world.h:83 averages, cellpool.h:243 applies erf on access).
        // Folding erf inside the average is not the same operator: erf is
        // concave, so by Jensen E[erf(x)] <= erf(E[x]) and the field reads low,
        // worst exactly where flow is bursty. It also destroyed the unsquashed
        // field the momentum denominator needs.
        g.vol_ema_b[i] = (1.f - lr) * g.vol_ema[i] + lr * g.vol_track[i];
        g.discharge_b[i] = std::erf(es * g.vol_ema_b[i]);
        g.Qm3s_b[i] = (1.f - lr) * g.Qm3s[i] +
                      lr * float(double(g.vol_track[i]) * q_per_unit_vol_m3_s);
        g.momx_b[i] = (1.f - lr) * g.momx[i] + lr * g.mx_track[i];
        g.momy_b[i] = (1.f - lr) * g.momy[i] + lr * g.my_track[i];
      }
    });
    g.vol_ema.swap(g.vol_ema_b);
    g.discharge.swap(g.discharge_b);
    g.Qm3s.swap(g.Qm3s_b);
    g.momx.swap(g.momx_b);
    g.momy.swap(g.momy_b);

    Diffuse(g, p, diff_demand, diff_limit, diff_delta);

    auto tD = clk::now(); t_grid += secs(tC, tD);
    // Water is a FIELD: accumulate it physically, then label the bodies. Both
    // every step -- there is no rebuild cadence any more.
    UpdateWater(g, p, step == 1 ? p.water_prime_iters : p.water_iters, water_flux);
    LabelWater(g, p, lakes, n_lakes, wet_frac, deepest_m);
    t_lake += secs(tD, clk::now());

    if (verbose && (step % p.snapshot_every == 0 || step == p.steps)) {
      char tag[64];
      std::snprintf(tag, sizeof(tag), "%04d-step", step);
      Dump(g, p, tag);
      float lo = 1e30f, hi = -1e30f;
      for (float h : g.height) { lo = std::min(lo, h); hi = std::max(hi, h); }
      std::vector<uint32_t> v;
      v.reserve(g.cells);
      for (uint32_t c : g.visits) if (c) v.push_back(c);
      std::sort(v.begin(), v.end());
      const double tot = double(std::accumulate(v.begin(), v.end(), 0ull));
      double hsum = 0.0;
      for (float h : g.height) hsum += double(h);
      std::printf("  step %4d/%d  relief %8.1f m  lakes %4d  wet %5.2f%%  "
                  "deepest %6.1f m\n"
                  "      mass: sum(h) %.4e  lake-dep %.4e  death-dep %.4e  "
                  "offmap %.4e\n"
                  "      visits/cell: touched %zu  mean %.2f  p50 %u  p99 %u  max %u\n",
                  step, p.steps, (hi - lo) * p.relief_m, n_lakes,
                  100.f * wet_frac, deepest_m, hsum, g.deposited_lake,
                  g.deposited_death, g.lost_offmap, v.size(),
                  v.empty() ? 0.0 : tot / double(v.size()),
                  v.empty() ? 0u : v[v.size() / 2],
                  v.empty() ? 0u : v[size_t(v.size() * 0.99)],
                  v.empty() ? 0u : v.back());
      std::fflush(stdout);
    }
  }

  st.n_lakes = n_lakes;
  st.wet_frac = wet_frac;
  st.deepest_m = deepest_m;
  st.t_drops = t_drops; st.t_grid = t_grid; st.t_lake = t_lake;
}

// ----------------------------------------------------------- world artifacts
//
// world.txt's manifest and rivers.bin's whole-world river graph -- the
// OUTPUT BOUNDARY this file exists to describe (see the header comment on the
// includes above). Nothing here feeds back into the sim; it only reads the
// FINISHED height/water/soil fields, in real metres, the same fields the
// .f32 dump already carries.

namespace mg = badlands::mapgen;

// Narrowest channel worth keeping in the whole-world graph -- the same
// physical threshold src/mapgen/file_patch_source.cpp culls a loaded network
// to (w = k_w*sqrt(Q) makes this Q >= 0.0036 m3/s): below it the network is a
// haze of hairlines. NOT the length prune -- a branch's length is only
// meaningful relative to a frame, and there is none yet at the whole-world
// scale; window.cpp/window_rivers.cpp apply that one per patch, later.
constexpr float kMinRiverWidthM = 0.3f;

// Same fractions tools/protogen/window.cpp's ClassifyBiomes cuts biomes with
// (kMountainFrac/kHillsFrac there). Duplicated rather than shared: window.cpp
// is its own standalone TU with its own build command, and this is a handful
// of lines -- see the README's TU-firewall note by the Field2D adapter below.
constexpr float kMountainFrac = 0.12f;
constexpr float kHillsFrac = 0.33f;

// mg::Field2D<float> differs from Grid's flat std::vector<float> at
// DIMENSIONLESS ~[0,1] scale (see the Grid struct's header note); this is the
// only place the two meet. Kept tiny and separate on purpose: the sim's own
// representation must not have to change shape to satisfy Field2D's, and this
// handful of lines is a firewall protecting it from that churn.
mg::Field2D<float> ToField2D(const std::vector<float>& v, int n, float scale) {
  mg::Field2D<float> f(n, n);
  for (size_t i = 0; i < v.size(); ++i) f.data[i] = v[i] * scale;
  return f;
}

// Whole-world quantiles of the DRY soil distribution, in metres -- the two
// numbers a patch cut later needs to classify biomes the same way regardless
// of what was cut, so it does not have to see the whole world to agree with
// it. Mirrors window.cpp's ClassifyBiomes exactly: same fractions, same "wet
// cells excluded" rule (a lake surface says nothing about the substrate under
// it).
void SoilCutoffs(const mg::Field2D<float>& soil_m, const mg::Field2D<float>& water_m,
                 float& t_mountain, float& t_hills) {
  std::vector<float> dry;
  dry.reserve(soil_m.data.size());
  for (size_t i = 0; i < soil_m.data.size(); ++i)
    if (water_m.data[i] <= 0.0f) dry.push_back(soil_m.data[i]);
  if (dry.empty()) { t_mountain = 0.0f; t_hills = 0.0f; return; }
  std::sort(dry.begin(), dry.end());
  const auto q = [&](float f) {
    return dry[std::min(dry.size() - 1, size_t(f * double(dry.size())))];
  };
  t_mountain = q(kMountainFrac);
  t_hills = q(kMountainFrac + kHillsFrac);
}

// What WriteWorldArtifacts needs, already in real metres -- built either from
// the live Grid right after RunSim (BuildInputsFromGrid, scaling by
// relief_m), or loaded back off an existing dump's .f32 rasters
// (RunExtractRivers, already metric on disk). Both feed the same function, so
// a normal run and `--extract-rivers` on its own output cannot diverge.
struct WorldArtifactInputs {
  int res = 0;
  float world_m = 0.0f;
  float runoff_m_per_yr = 0.0f;
  uint32_t seed = 0;
  int steps = 0;
  mg::Field2D<float> height_m;  // bed + soil, dry-land surface
  mg::Field2D<float> water_m;   // standing water depth
  mg::Field2D<float> soil_m;    // erodible cover thickness
};

// Writes world.txt then rivers.bin to `out_dir`. The manifest goes first
// because write_coarse_manifest is what creates `out_dir` if it does not
// exist yet; write_river_graph does not.
//
// No lake_id/LakeInfo is threaded through here: extract_river_graph derives
// its own lake components from water_m > 0 internally (the optional lake_id/
// lakes parameters exist only to report LakeKind::Seeded provenance, and
// protogen has no seeded lakes -- every one of its lakes is Emergent by
// construction -- so the default is already the right answer).
bool WriteWorldArtifacts(const WorldArtifactInputs& in, const std::string& out_dir) {
  float t_mountain = 0.0f, t_hills = 0.0f;
  SoilCutoffs(in.soil_m, in.water_m, t_mountain, t_hills);

  mg::CoarseManifest man;
  man.resolution = in.res;
  man.world_size_m = in.world_m;
  man.texel_m = in.res > 0 ? in.world_m / float(in.res) : 0.0f;
  man.seed = in.seed;
  man.runoff_m_per_yr = in.runoff_m_per_yr;
  man.steps = in.steps;
  man.soil_cut_mountain_m = t_mountain;
  man.soil_cut_hills_m = t_hills;

  std::string err;
  if (!mg::write_coarse_manifest(out_dir, man, &err)) {
    std::fprintf(stderr, "protogen: %s\n", err.c_str());
    return false;
  }

  // Route on bed + standing water, exactly what build_window_rivers does for
  // a patch (window_rivers.cpp) -- a lake surface is where flow actually
  // travels, not a hole in the terrain. No ghost padding: unlike a windowed
  // cutout, the whole world's own border genuinely IS base level.
  const float texel_m = man.texel_m;
  mg::Field2D<float> surface = in.height_m;
  for (size_t i = 0; i < surface.data.size(); ++i)
    surface.data[i] += in.water_m.data[i];
  mg::Field2D<uint8_t> lake_tag(in.res, in.res, 0);
  for (size_t i = 0; i < lake_tag.data.size(); ++i)
    lake_tag.data[i] = in.water_m.data[i] > 0.0f ? 1 : 0;

  const mg::FlowRouting routing =
      mg::route_flow(surface, texel_m, mg::kEpsilonM, &lake_tag);
  const mg::Field2D<float> area =
      mg::accumulate_drainage(routing, texel_m * texel_m);

  mg::ErosionParams ep;  // defaults, except runoff -- matched to the sim's own rate
  ep.runoff_m_per_s = float(double(in.runoff_m_per_yr) / kSecondsPerYear);

  mg::RiverGraph graph = mg::extract_river_graph(routing, area, in.water_m,
                                                 surface, ep, texel_m, 0.0f);
  // ONLY the width prune -- see kMinRiverWidthM above for why the length
  // prune does not belong here.
  mg::prune_river_graph_by_width(graph, kMinRiverWidthM);

  if (!mg::write_river_graph(out_dir + "/rivers.bin", graph, &err)) {
    std::fprintf(stderr, "protogen: %s\n", err.c_str());
    return false;
  }
  std::printf("protogen: wrote %s/world.txt + rivers.bin (%zu nodes, %zu edges)\n",
              out_dir.c_str(), graph.nodes.size(), graph.edges.size());
  return true;
}

WorldArtifactInputs BuildInputsFromGrid(const Grid& g, const Params& p) {
  WorldArtifactInputs in;
  in.res = g.n;
  in.world_m = p.world_m;
  in.runoff_m_per_yr = p.runoff_m_per_yr;
  in.seed = p.seed;
  in.steps = p.steps;
  in.height_m = ToField2D(g.height, g.n, p.relief_m);
  in.water_m = ToField2D(g.water, g.n, p.relief_m);
  in.soil_m = ToField2D(g.soil, g.n, p.relief_m);
  return in;
}

// Reads exactly n*n floats off `path` -- the same headerless .f32 form Dump()
// writes -- into a right-sized vector. A size mismatch (wrong --res, or a
// dump that predates the substrate) is reported, not guessed.
bool LoadDumpField(const std::string& path, int n, std::vector<float>& out) {
  out.assign(size_t(n) * n, 0.0f);
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    std::fprintf(stderr, "protogen: cannot open %s\n", path.c_str());
    return false;
  }
  const size_t want = out.size();
  const size_t got = std::fread(out.data(), sizeof(float), want, fp);
  std::fclose(fp);
  if (got != want) {
    std::fprintf(stderr, "protogen: %s is %zu floats, expected %zu (wrong --res?)\n",
                 path.c_str(), got, want);
    return false;
  }
  return true;
}

// `--extract-rivers <dir>`: re-runs ONLY the extraction + serialization
// above against an EXISTING dump, and exits. Exists so an extraction bug
// costs seconds instead of the several-minute re-sim a full run takes -- and
// because it calls the exact same WriteWorldArtifacts a normal run does, the
// two paths cannot silently diverge.
//
// Reads the FINAL snapshot: RunSim always dumps at step == p.steps regardless
// of --snapshot-every (see its verbose block), so world.txt's own `steps`
// names that tag exactly.
int RunExtractRivers(const std::string& dir) {
  std::string err;
  const auto man = mg::load_coarse_manifest(dir, &err);
  if (!man) {
    std::fprintf(stderr, "protogen: --extract-rivers: %s\n", err.c_str());
    return 1;
  }

  char tag[64];
  std::snprintf(tag, sizeof(tag), "%04d-step", man->steps);
  const std::string base = dir + "/" + std::string(tag) + "-";

  WorldArtifactInputs in;
  in.res = man->resolution;
  in.world_m = man->world_size_m;
  in.runoff_m_per_yr = man->runoff_m_per_yr;
  in.seed = man->seed;
  in.steps = man->steps;

  std::vector<float> height, water, soil;
  if (!LoadDumpField(base + "height.f32", in.res, height)) return 1;
  if (!LoadDumpField(base + "water.f32", in.res, water)) return 1;
  if (!LoadDumpField(base + "soil.f32", in.res, soil)) return 1;

  in.height_m = mg::Field2D<float>(in.res, in.res);
  in.height_m.data = std::move(height);
  in.water_m = mg::Field2D<float>(in.res, in.res);
  in.water_m.data = std::move(water);
  in.soil_m = mg::Field2D<float>(in.res, in.res);
  in.soil_m.data = std::move(soil);

  return WriteWorldArtifacts(in, dir) ? 0 : 1;
}

// ---------------------------------------------------------------------- tests

// Physical sanity tests. Every one runs on a SMALL grid (32-64 cells) but at
// the production cell size (16 m), so the physics sees representative distances
// and only the extent shrinks -- a test that passed at 1 m cells would prove
// nothing about a 16 m world.
//
// Each test isolates ONE mechanism using the enable_* toggles, so a failure has
// a single cause. Where an analytic answer exists it is asserted against, not
// merely eyeballed.

namespace test {

int g_pass = 0, g_fail = 0, g_pending = 0, g_pending_ready = 0;

void Check(const char* name, bool ok, const std::string& detail) {
  std::printf("  [%s] %-38s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (ok) ++g_pass; else ++g_fail;
}

// An invariant for a mechanism that is not built yet.
//
// These are written BEFORE their phase, so they start red on purpose and must
// not break --test as a regression gate. Reported, never fatal. When the owning
// phase lands, flip the call to Check() and the contract becomes enforced.
//
// PEND! means it passed unexpectedly -- either the mechanism arrived or the
// assertion is too weak to be worth having. Both are worth looking at.
void Pending(const char* name, bool ok, const std::string& detail) {
  std::printf("  [%s] %-38s %s\n", ok ? "PEND!" : "PEND ", name, detail.c_str());
  ++g_pending;
  if (ok) ++g_pending_ready;
}

std::string F(const char* fmt, double a, double b = 0, double c = 0) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}

// A small world at production cell size: 64 cells x 16 m = 1024 m.
Params Base(int res = 64) {
  Params p;
  p.res = res;
  p.world_m = 16.0f * float(res);
  p.relief_m = 300.0f;
  p.steps = 200;
  p.drops = 16;
  p.snapshot_every = 1 << 30;  // never
  p.lake_interval = 25;  // production default: tests must match it
  return p;
}

Grid Run(const Params& p, std::vector<Lake>& lakes, SimStats& st) {
  Grid g(p.res);
  InitTerrain(g, p);
  RunSim(p, g, lakes, st, false);
  return g;
}

double SumH(const Grid& g) {
  double s = 0;
  for (float h : g.height) s += double(h);
  return s;
}

// --- 1. mass conservation -------------------------------------------------
// Everything the particles move must be accounted for: what stays on the grid
// plus what left the map equals what was there to begin with.
void MassConservation() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.prefill = true;
  Grid g0(p.res);
  InitTerrain(g0, p);
  const double before = SumH(g0);
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  const double after = SumH(g);
  const double residual = (after - before) + g.lost_offmap;
  const double rel = std::fabs(residual) / std::max(std::fabs(before), 1e-9);
  Check("mass conservation", rel < 0.01,
        F("residual %.3e of %.3e (%.4f%%)", residual, before, 100 * rel));
}

// --- 2. flat plane does nothing -------------------------------------------
// Zero slope means zero transport capacity, so nothing may erode or deposit.
void FlatPlaneInert() {
  Params p = Base();
  p.terrain = Params::Terrain::Flat;
  Grid g0(p.res);
  InitTerrain(g0, p);
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  float max_change = 0.f;
  for (size_t i = 0; i < g.cells; ++i)
    max_change = std::max(max_change,
                          std::fabs(g.height[i] - g0.height[i]) * p.relief_m);
  Check("flat plane stays flat", max_change < 0.5f,
        F("max change %.4f m", max_change));
}

// --- 3. determinism --------------------------------------------------------
void Determinism() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  std::vector<Lake> l1, l2; SimStats s1, s2;
  Grid a = Run(p, l1, s1);
  Grid b = Run(p, l2, s2);
  bool same = true;
  for (size_t i = 0; i < a.cells && same; ++i)
    if (a.height[i] != b.height[i]) same = false;
  Check("same seed is bit-identical", same, same ? "identical" : "DIVERGED");
}

// --- 4. knob liveness ------------------------------------------------------
// Two distinct values of a parameter must NOT give bit-identical output.
// Both masking bugs found so far (the lobe-length clamp hiding
// settling_velocity, the wander cap hiding jet turbulence) announced themselves
// exactly this way, and both were spotted by eye, late.
void KnobLiveness() {
  struct Knob { const char* name; void (*set)(Params&, int); };
  static const Knob knobs[] = {
      {"settling_velocity", [](Params& p, int i) {
         p.settling_velocity_m_per_s = i ? 1e-4f : 1e-1f; }},
      {"plume_wander_deg", [](Params& p, int i) {
         p.plume_wander_deg = i ? 0.f : 60.f; }},
      {"entrainment", [](Params& p, int i) { p.entrainment = i ? 2.f : 20.f; }},
      {"adaptation_length", [](Params& p, int i) {
         p.adaptation_length_m = i ? 60.f : 600.f; }},
      {"repose_angle_deg", [](Params& p, int i) {
         p.repose_angle_deg = i ? 15.f : 60.f; }},
      {"evaporation", [](Params& p, int i) {
         p.evaporation_m_per_yr = i ? 0.05f : 5.f; }},
      {"runoff", [](Params& p, int i) { p.runoff_m_per_yr = i ? 0.1f : 4.f; }},
  };
  for (const Knob& k : knobs) {
    Params a = Base(), b = Base();
    a.terrain = b.terrain = Params::Terrain::Bowl;
    a.prefill = b.prefill = true;
    k.set(a, 0); k.set(b, 1);
    std::vector<Lake> la, lb; SimStats sa, sb;
    Grid ga = Run(a, la, sa), gb = Run(b, lb, sb);
    bool differs = false;
    for (size_t i = 0; i < ga.cells && !differs; ++i)
      if (ga.height[i] != gb.height[i]) differs = true;
    Check((std::string("knob is live: ") + k.name).c_str(), differs,
          differs ? "output changes" : "NO EFFECT - masked or unused");
  }
}

// --- 5. lake surface is level ---------------------------------------------
// Every wet cell of one lake must share a water surface. Catches the stale
// water field that let a bed rise while still reporting itself as lake bottom.
void LakeFlatness() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.prefill = true;
  // Terrain frozen: this asserts lake BOOKKEEPING. With erosion live the bowl
  // is marginal enough that a rebuild can find the basin breached and drop the
  // lake, which is real behaviour but would make this test measure erosion.
  p.enable_erosion = false;
  p.enable_cascade = false;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  std::vector<float> lo(g.lake_outlet.size(), 1e30f), hi(g.lake_outlet.size(), -1e30f);
  size_t wet = 0;
  for (size_t i = 0; i < g.cells; ++i) {
    if (g.lake_id[i] < 0 || g.water[i] <= 0.f) continue;
    const float surf = g.height[i] + g.water[i];
    lo[g.lake_id[i]] = std::min(lo[g.lake_id[i]], surf);
    hi[g.lake_id[i]] = std::max(hi[g.lake_id[i]], surf);
    ++wet;
  }
  float worst = 0.f;
  for (size_t k = 0; k < lo.size(); ++k)
    if (hi[k] > -1e29f) worst = std::max(worst, (hi[k] - lo[k]) * p.relief_m);
  Check("lake surface is level", wet > 0 && worst < 0.05f,
        F("%.0f wet cells, worst spread %.4f m", double(wet), worst));
}

// --- 6. lake_id resolves ---------------------------------------------------
// lake_id holds the KEPT index while the Lake vector is unpruned; mixing them
// debited the wrong lake's volume.
void LakeIdValid() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.prefill = true;
  // Terrain frozen: this asserts lake BOOKKEEPING. With erosion live the bowl
  // is marginal enough that a rebuild can find the basin breached and drop the
  // lake, which is real behaviour but would make this test measure erosion.
  p.enable_erosion = false;
  p.enable_cascade = false;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  bool ok = true;
  for (size_t i = 0; i < g.cells && ok; ++i) {
    const int32_t id = g.lake_id[i];
    if (id < 0) continue;
    if (id >= int32_t(g.lake_outlet.size()) ||
        id >= int32_t(g.lake_index.size())) ok = false;
  }
  Check("every lake_id resolves", ok,
        F("%.0f lakes", double(g.lake_outlet.size())));
}

// --- 7. lakes never cover the map -----------------------------------------
// A fixture whose interior is the global minimum floods entirely; that is a
// broken fixture, not a lake.
void LakeAreaBounded() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.prefill = true;
  // Terrain frozen: this asserts lake BOOKKEEPING. With erosion live the bowl
  // is marginal enough that a rebuild can find the basin breached and drop the
  // lake, which is real behaviour but would make this test measure erosion.
  p.enable_erosion = false;
  p.enable_cascade = false;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  size_t wet = 0;
  for (float w : g.water) if (w > 0.f) ++wet;
  const double frac = double(wet) / double(g.cells);
  Check("lake area below half the map", frac < 0.5,
        F("wet %.2f%%", 100 * frac));
}

// --- 8. discharge equals runoff x drainage area ---------------------------
// The strongest statement the discharge field makes about itself.
void DischargeMatchesArea() {
  Params p = Base();
  p.terrain = Params::Terrain::Plane;
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.steps = 400;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  // Every particle drains down +y, so the bottom row carries the whole map.
  double q_bottom = 0.0;
  for (int x = 1; x < g.n - 1; ++x) q_bottom += g.Qm3s[g.idx(x, g.n - 2)];
  const double area = double(p.world_m) * double(p.world_m);
  const double expect = double(p.runoff_m_per_yr) * area / kSecondsPerYear;
  const double ratio = expect > 0 ? q_bottom / expect : 0;
  Check("discharge ~ runoff x area", ratio > 0.2 && ratio < 5.0,
        F("measured %.4e vs expected %.4e (ratio %.2f)", q_bottom, expect, ratio));
}

// --- 9. valley concentrates the flow --------------------------------------
// z = a*y + b*|x| -- the channel must form in the central column.
void ValleyChannel() {
  Params p = Base();
  p.terrain = Params::Terrain::Valley;
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.steps = 400;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  // Column totals of discharge across the lower half of the map.
  std::vector<double> col(g.n, 0.0);
  for (int y = g.n / 2; y < g.n - 1; ++y)
    for (int x = 0; x < g.n; ++x) col[x] += g.Qm3s[g.idx(x, y)];
  const int argmax = int(std::max_element(col.begin(), col.end()) - col.begin());
  double total = 0, near = 0;
  for (int x = 0; x < g.n; ++x) {
    total += col[x];
    if (std::abs(x - g.n / 2) <= 2) near += col[x];
  }
  const double share = total > 0 ? near / total : 0;
  Check("valley channels the flow",
        std::abs(argmax - g.n / 2) <= 2 && share > 0.5,
        F("peak col %.0f (centre %.0f), %.0f%% within +/-2",
          double(argmax), double(g.n / 2), 100 * share));
}

// --- 10. cascade relaxes a cliff to the repose angle ----------------------
void ReposeAngle() {
  Params p = Base();
  p.terrain = Params::Terrain::Cliff;
  p.enable_erosion = false;
  p.steps = 600;
  p.drops = 64;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  const float cell_m = p.world_m / float(p.res);
  float max_slope_deg = 0.f;
  for (int y = 1; y < g.n - 1; ++y)
    for (int x = 1; x < g.n - 1; ++x) {
      const float dz = std::fabs(g.height[g.idx(x, y)] -
                                 g.height[g.idx(x, y + 1)]) * p.relief_m;
      max_slope_deg = std::max(max_slope_deg,
                               float(std::atan(dz / cell_m) * 180.0 / 3.14159265));
    }
  // The cascade is a LOCAL, per-particle operation: it fires only at cells a
  // particle visits, so it cannot promise a global repose bound and untouched
  // cliff survives at 90 deg. What it does promise is that it relaxes what it
  // touches, so assert the maximum slope fell.
  float initial_deg = 0.f;
  {
    Grid g0(p.res);
    InitTerrain(g0, p);
    for (int y = 1; y < g0.n - 1; ++y)
      for (int x = 1; x < g0.n - 1; ++x) {
        const float dz = std::fabs(g0.height[g0.idx(x, y)] -
                                   g0.height[g0.idx(x, y + 1)]) * p.relief_m;
        initial_deg = std::max(initial_deg,
                               float(std::atan(dz / cell_m) * 180.0 / 3.14159265));
      }
  }
  Check("cascade relaxes a cliff", max_slope_deg < initial_deg,
        F("max slope %.1f -> %.1f deg (repose %.1f)", initial_deg,
          max_slope_deg, p.repose_angle_deg));
}

// --- substrate: bedrock actually resists ----------------------------------
// The whole point of the two-layer substrate. Same fixture, same forcing, only
// the starting soil differs: a deeply-mantled hill must lose substantially more
// material than a bare-rock one.
void BedrockResists() {
  Params soft = Base(), hard = Base();
  soft.terrain = hard.terrain = Params::Terrain::Lobe;
  soft.steps = hard.steps = 200;
  soft.initial_soil_m = 500.0f;  // effectively unlimited cover
  hard.initial_soil_m = 0.0f;    // bare rock from the first step
  std::vector<Lake> ls, lh; SimStats ss, sh;
  Grid g0(soft.res);
  InitTerrain(g0, soft);
  Grid gs = Run(soft, ls, ss), gh = Run(hard, lh, sh);
  double base = 0, vs = 0, vh = 0;
  for (size_t i = 0; i < gs.cells; ++i) {
    base += g0.height[i];
    vs += gs.height[i];
    vh += gh.height[i];
  }
  const double lost_soft = base - vs, lost_hard = base - vh;
  // Not asserting the exact 10x: the cascade, the lakes and the death deposits
  // all put material back, so the ratio at the landscape scale is softer than
  // the per-cut one. What must hold is a clear, ordered difference.
  Check("substrate: bedrock resists erosion", lost_hard < lost_soft * 0.75,
        F("lost soft %.4e vs bare rock %.4e (ratio %.2f)", lost_soft, lost_hard,
          lost_soft > 0 ? lost_hard / lost_soft : 0.0));
}

// --- substrate: inert while soil lasts ------------------------------------
// A real logic change that must produce BIT-IDENTICAL output -- the diagnostic
// that has caught every masked knob in this work. With cover deeper than the run
// can ever cut through, bedrock hardness is unreachable, so varying it must
// change nothing. If this ever differs, the substrate is biting when it should
// not be.
void SubstrateInertWhileSoilLasts() {
  Params a = Base(), b = Base();
  a.terrain = b.terrain = Params::Terrain::Lobe;
  a.steps = b.steps = 120;
  a.initial_soil_m = b.initial_soil_m = 1000.0f;
  a.bedrock_erodibility = 1.0f;   // as soft as soil
  b.bedrock_erodibility = 1e-6f;  // effectively unbreakable
  std::vector<Lake> la, lb; SimStats sa, sb;
  Grid ga = Run(a, la, sa), gb = Run(b, lb, sb);
  size_t diff = 0;
  for (size_t i = 0; i < ga.cells; ++i)
    if (ga.height[i] != gb.height[i]) ++diff;
  Check("substrate: inert while soil lasts", diff == 0,
        diff == 0 ? "bit-identical" : F("%.0f cells differ", double(diff)));
}

// --- substrate: soil is conserved and never negative ----------------------
// height == bedrock + soil is the invariant every mutation site must preserve.
// A negative soil would mean Erode took more cover than existed.
void SoilStaysPhysical() {
  Params p = Base();
  p.terrain = Params::Terrain::Lobe;
  p.steps = 250;
  std::vector<Lake> lk; SimStats st;
  Grid g = Run(p, lk, st);
  float worst = 0.f;
  size_t bare = 0;
  for (float s : g.soil) {
    worst = std::min(worst, s);
    if (s <= 0.f) ++bare;
  }
  // Some cells MUST reach bare rock, or the fixture never exercised the
  // bedrock path and the test above proves nothing.
  Check("substrate: soil stays physical", worst >= 0.f && bare > 0,
        F("min soil %.6f m, %.0f of %.0f cells stripped to rock",
          worst * p.relief_m, double(bare), double(g.cells)));
}

// --- substrate: deposition builds soil ------------------------------------
// Material that settles is loose by definition. A basin that receives fill must
// end with MORE cover than it started with, or deposition is writing to bedrock.
void DepositionBuildsSoil() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.steps = 200;
  std::vector<Lake> lk; SimStats st;
  Grid g0(p.res);
  InitTerrain(g0, p);
  Grid g = Run(p, lk, st);
  // Compare where the surface rose: those cells received fill.
  double gained_height = 0, gained_soil = 0;
  for (size_t i = 0; i < g.cells; ++i) {
    const float dh = g.height[i] - g0.height[i];
    if (dh > 0.f) {
      gained_height += dh;
      gained_soil += g.soil[i] - g0.soil[i];
    }
  }
  Check("substrate: deposition builds soil", gained_height > 0 && gained_soil > 0,
        F("surface +%.4e, soil +%.4e over filled cells", gained_height,
          gained_soil));
}

// --- 11. erosion lowers a hill --------------------------------------------
void LobeErodes() {
  Params p = Base();
  p.terrain = Params::Terrain::Lobe;
  p.steps = 100;
  std::vector<Lake> lakes; SimStats st;
  Grid g0(p.res);
  InitTerrain(g0, p);
  float peak0 = 0.f;
  for (float h : g0.height) peak0 = std::max(peak0, h);
  float prev = peak0;
  bool monotone = true;
  for (int round = 0; round < 4; ++round) {
    Params q = p;
    q.steps = 100 * (round + 1);
    std::vector<Lake> lk; SimStats s;
    Grid g = Run(q, lk, s);
    float peak = 0.f;
    for (float h : g.height) peak = std::max(peak, h);
    if (peak > prev + 1e-6f) monotone = false;
    prev = peak;
  }
  // NOTE: the summit itself is expected to survive. A hilltop has no upslope
  // contributing area, so fluvial incision cannot reach it -- only hillslope
  // diffusion (creep) lowers summits, and this model has none. So assert what
  // erosion genuinely does: it removes mass from the flanks.
  Params q = p; q.steps = 400;
  std::vector<Lake> lk; SimStats s2;
  Grid ge = Run(q, lk, s2);
  double v0 = 0, v1 = 0;
  for (size_t i = 0; i < ge.cells; ++i) { v0 += g0.height[i]; v1 += ge.height[i]; }
  Check("erosion removes mass from the hill", v1 < v0 && monotone,
        F("mass %.4e -> %.4e (peak %.1f m held)", v0, v1, prev * p.relief_m));
}


// --- 13. symmetric fixture, symmetric result ------------------------------
void Symmetry() {
  Params p = Base();
  p.terrain = Params::Terrain::Lobe;
  p.steps = 150;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  // Mirror about x and compare column sums; the fixture and the noise-free
  // spawn are both x-symmetric, so a large asymmetry means a directional bias
  // (e.g. the cascade's fixed neighbour ordering).
  double asym = 0, tot = 0;
  for (int y = 1; y < g.n - 1; ++y)
    for (int x = 1; x < g.n / 2; ++x) {
      const double a = g.height[g.idx(x, y)];
      const double b = g.height[g.idx(g.n - 1 - x, y)];
      asym += std::fabs(a - b);
      tot += std::fabs(a) + std::fabs(b);
    }
  const double rel = tot > 0 ? asym / tot : 0;
  Check("no gross left/right bias", rel < 0.25, F("asymmetry %.2f%%", 100 * rel));
}


// --- 15. resolution independence ------------------------------------------
// Same world, same cell size, different extent must not change the character.
// Several constants are per-cell, so cell size can leak into the physics.
void ResolutionIndependence() {
  double dens[2];
  for (int i = 0; i < 2; ++i) {
    Params p = Base(i ? 64 : 32);
    p.terrain = Params::Terrain::Plane;
    p.steps = 300;
    std::vector<Lake> lakes; SimStats st;
    Grid g = Run(p, lakes, st);
    // Channel density: fraction of cells above a fixed discharge quantile.
    std::vector<float> q(g.Qm3s);
    std::sort(q.begin(), q.end());
    const float thr = q[size_t(q.size() * 0.9)];
    size_t ch = 0;
    for (float v : g.Qm3s) if (v > thr && thr > 0) ++ch;
    dens[i] = double(ch) / double(g.cells);
  }
  const double rel = dens[0] > 0 ? std::fabs(dens[1] - dens[0]) / dens[0] : 1;
  Check("channel density independent of extent", rel < 0.5,
        F("32-cell %.4f vs 64-cell %.4f (%.0f%% apart)", dens[0], dens[1],
          100 * rel));
}

// --- 16. delta thins with distance ----------------------------------------
void DeltaProfile() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.prefill = true;
  // Basin deep enough to survive 300 steps of incision but shallow enough to
  // be a lake (23 m at the end, not 260 m). And a grain COARSE enough to settle
  // inside it: L = u*h/w_s = 0.08*23/0.05 = 37 m against a ~110 m lake. With
  // the default fine sand L is 184 m, longer than the lake, so the load
  // transits and no delta can form -- which is the whole point of the test.
  p.bowl_well_m = 120.0f;
  p.settling_velocity_m_per_s = 0.05f;
  p.steps = 300;
  Grid g0(p.res);
  InitTerrain(g0, p);
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  // Distance is measured from the ACTUAL INLET -- the wet cell carrying the
  // most discharge -- not from the top of the basin. The basin's upstream edge
  // is dry land above the waterline, so banding by it put band 0 entirely on
  // ground the plume never touches and reported 0.0 m every time.
  const float cell = p.world_m / float(p.res);
  int inlet = -1;
  float best_q = -1.f;
  for (size_t i = 0; i < g.cells; ++i)
    if (g.water[i] > 0.f && g.Qm3s[i] > best_q) { best_q = g.Qm3s[i]; inlet = int(i); }
  if (inlet < 0) {
    // The delta may have filled the lake; fall back to the deposit's own
    // upstream end within the basin.
    for (size_t i = 0; i < g.cells; ++i)
      if (g.height[i] > g0.height[i] + 1e-6f) { inlet = int(i); break; }
  }
  if (inlet < 0) { Check("delta thins with distance from inlet", false, "no deposit"); return; }
  const int ix = inlet % g.n, iy = inlet / g.n;

  // Mean deposit thickness in four annuli out from the inlet.
  std::vector<double> band(4, 0.0);
  std::vector<int> cnt(4, 0);
  double rmax = 0;
  for (size_t i = 0; i < g.cells; ++i) {
    if (g.height[i] <= g0.height[i] + 1e-6f) continue;
    const double dx = double(int(i) % g.n - ix), dy = double(int(i) / g.n - iy);
    rmax = std::max(rmax, std::sqrt(dx * dx + dy * dy));
  }
  if (rmax <= 0) { Check("delta thins with distance from inlet", false, "no deposit"); return; }
  for (size_t i = 0; i < g.cells; ++i) {
    const double d = (g.height[i] - g0.height[i]) * p.relief_m;
    if (d <= 0) continue;
    const double dx = double(int(i) % g.n - ix), dy = double(int(i) / g.n - iy);
    const double r = std::sqrt(dx * dx + dy * dy);
    const int bi = std::min(3, int(4.0 * r / (rmax + 1e-9)));
    band[bi] += d;
    ++cnt[bi];
  }
  for (int bi = 0; bi < 4; ++bi) if (cnt[bi]) band[bi] /= cnt[bi];
  const bool thins = band[0] >= band[3];
  (void)cell;
  Check("delta thins with distance from inlet", thins,
        F("near %.2f m -> far %.2f m (peak %.2f)", band[0], band[3],
          *std::max_element(band.begin(), band.end())));
}


// ==========================================================================
// PHYSICS INVARIANTS
//
// Written BEFORE the mechanisms they describe, so most start red and report
// PEND rather than FAIL. They are the contract each phase is finished against;
// a phase is done when its invariant flips from Pending() to Check() and
// stays green. Runs on real terrain are the LAST step of a phase, never the
// evidence it worked.
// ==========================================================================

// --- T1. terminal velocity matches Manning --------------------------------
// A particle on a constant incline must accelerate under gravity until drag
// balances it, settling at Manning's u = h^(2/3)*sqrt(S)/n -- the same closure
// src/mapgen/river_graph.cpp already uses for the river graph. Today the motion
// law renormalises speed to sqrt(2) cells/step every step, so there is neither
// a velocity in m/s nor a flow depth to put in that formula.
void TerminalVelocityManning() {
  Params p = Base(32);
  p.terrain = Params::Terrain::Plane;
  const float slope = 0.02f;  // 2%: Plane is h = rim_m*(1 - y/world_m)
  p.bowl_rim_m = slope * p.world_m;
  p.enable_erosion = false;  // isolate the MOTION law
  p.enable_cascade = false;
  p.enable_lake_deposit = false;
  p.steps = 1;
  p.drops = 1;  // exactly one particle, so the probe is one trajectory
  ParticleProbe probe;
  p.probe = &probe;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);

  const float n_manning = 0.035f;  // ErosionParams::manning_n
  const size_t n = probe.speed.size();
  const float depth_m = n ? probe.depth_m.back() : 0.f;
  const float measured = n ? probe.speed.back() : 0.f;
  const float expect = (depth_m > 0.f)
                           ? std::pow(depth_m, 2.f / 3.f) * std::sqrt(slope) /
                                 n_manning
                           : 0.f;
  // Converged means the last quarter of the trajectory is flat.
  bool converged = n >= 8;
  if (converged)
    for (size_t i = n - n / 4; i < n; ++i)
      if (std::fabs(probe.speed[i] - measured) > 0.01f * std::max(measured, 1e-6f))
        converged = false;
  const bool ok = depth_m > 0.f && converged &&
                  std::fabs(measured - expect) <= 0.1f * expect;
  std::string detail;
  if (depth_m <= 0.f) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "no flow depth or time axis: speed pinned at %.3f cells/step "
                  "over %zu steps", measured, n);
    detail = buf;
  } else {
    detail = F("u %.3f vs Manning %.3f m/s (h %.2f m)", measured, expect,
               depth_m);
  }
  // ENFORCED as of the drag/Manning motion law.
  Check("T1 terminal velocity = Manning", ok, detail);
}

// --- T2. particle volume is pure discretization ---------------------------
// Total water is drops x drop_volume. Chopping the same water into a different
// number of parcels is a NUMERICAL choice and must not change the landscape.
// Compared on aggregate statistics, not per cell: halving the drop count
// changes the spawn sequence, so the fields cannot be pointwise identical and
// asserting that would make the invariant unpassable by construction.
void VolumeDiscretizationInvariance() {
  // Metrics must measure the EROSION, not the terrain. Relief is set almost
  // entirely by the starting fixture, so comparing it agrees to 4 digits
  // whatever the particles did -- it passes without testing anything.
  auto stats = [](const Params& base, int drops, float vol) {
    Params p = base;
    p.drops = drops;
    p.drop_volume = vol;
    Grid g0(p.res);
    InitTerrain(g0, p);
    std::vector<Lake> lakes; SimStats st;
    Grid g = Run(p, lakes, st);
    double moved = 0.0;
    for (size_t i = 0; i < g.cells; ++i)
      moved += std::fabs(double(g.height[i]) - double(g0.height[i]));
    return std::array<double, 3>{
        moved / double(g.cells) * base.relief_m,  // mean |dh|, metres
        double(g.deposited_death + g.deposited_lake),
        double(g.lost_offmap)};
  };
  Params base = Base(48);
  base.terrain = Params::Terrain::Bowl;
  base.steps = 120;
  auto rel = [](double x, double y) {
    return std::fabs(x - y) / std::max(std::fabs(x), 1e-9);
  };
  // CONVERGENCE, not equality -- the same shape as RebuildCadenceInvariant.
  //
  // Exact agreement is not the right ask: erosion is nonlinear (c_eq saturates,
  // Erode is limited by available soil), so a few fat parcels sample the
  // terrain more coarsely than many thin ones and land somewhere different.
  // What must hold is that REFINING the discretization stops changing the
  // answer. Total water is drops*drop_volume and is identical across all three.
  const auto coarse = stats(base, 32, 4.0f);
  const auto mid = stats(base, 64, 2.0f);
  const auto fine = stats(base, 128, 1.0f);
  const double d_cm = rel(coarse[0], mid[0]), d_mf = rel(mid[0], fine[0]);
  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "mean |dh| %.3f / %.3f / %.3f m at 32x4 / 64x2 / 128x1; "
                "successive %.0f%% then %.0f%%",
                coarse[0], mid[0], fine[0], 100 * d_cm, 100 * d_mf);
  // ENFORCED once the update is resolved PER CELL along the swept segment.
  // Bound is 10%, widened from 5% when lrate moved to the reference's 0.1.
  // At the slower 0.01 this read 5% then 2%; at 0.1 the discharge EMA averages
  // over ~10 steps instead of ~100, so it is a noisier estimator and its noise
  // depends on how many parcels sampled it. That residual is NOISE, not drift:
  // resampling with 2x the particles and steps gave 28% then 22% and went
  // NON-monotone (1.903 / 2.436 / 1.900), which drift does not do.
  //
  // The bound still catches what this test exists for. The defects it found
  // were 47%/31% (per-iteration relaxation, logarithmic and never converging)
  // and 33%/23% (capacity shared across a multi-cell step) -- both far outside
  // 10% and both monotone.
  // BOTH successive differences under the bound -- not "each smaller than the
  // last". Requiring monotone shrinkage tests which way the noise fell, and at
  // an ~8% noise floor that is a coin toss (measured 7% then 8%).
  Check("T2 volume is pure discretization", d_cm < 0.10 && d_mf < 0.10, buf);
}

// --- T3. diffusion relaxes a ridge and conserves mass ---------------------
// Soil creep on a 1-D gaussian ridge, with NO particles and no cascade, so the
// only thing that can move material is the diffusion pass. The peak must fall,
// the flanks must rise, and nothing may be created or destroyed.
void DiffusionRelaxesRidge() {
  Params p = Base(64);
  p.terrain = Params::Terrain::Ridge;
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.enable_lake_deposit = false;
  p.enable_diffusion = true;
  p.diffusion_D_m2_per_yr = 5e-3f;
  p.steps = 50;
  p.drops = 0;  // pure grid pass
  Grid g0(p.res);
  InitTerrain(g0, p);
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);

  const int mid = p.res / 2;
  // Peak column and a flank column well off the crest.
  const int crest = mid, flank = mid + p.res / 6;
  const float peak0 = g0.height[g0.idx(crest, mid)];
  const float peak1 = g.height[g.idx(crest, mid)];
  const float side0 = g0.height[g0.idx(flank, mid)];
  const float side1 = g.height[g.idx(flank, mid)];
  const double m0 = SumH(g0), m1 = SumH(g);
  const double mass_rel = std::fabs(m1 - m0) / std::max(std::fabs(m0), 1e-9);
  const bool ok = peak1 < peak0 && side1 > side0 && mass_rel < 1e-4;
  // ENFORCED as of the hillslope diffusion pass.
  Check("T3 diffusion relaxes a ridge", ok,
        F("peak %+.3f m, flank %+.3f m, mass drift %.2e",
            double(peak1 - peak0) * p.relief_m,
            double(side1 - side0) * p.relief_m, mass_rel));
}

// --- T4. a particle may never skip a cell ---------------------------------
// The anti-tunnelling property, and the precondition for swept-path writes:
// if a step lands two cells away, whatever it deposited along the way went
// nowhere. Today speed is pinned at sqrt(2) cells/step, which overshoots a
// cell boundary whenever the particle does not start near one.
void ParticleNeverSkipsACell() {
  Params p = Base(48);
  p.terrain = Params::Terrain::Plane;
  p.bowl_rim_m = 0.10f * p.world_m;  // 10%: fast, so skipping is likely
  p.enable_cascade = false;
  p.steps = 1;
  p.drops = 1;
  ParticleProbe probe;
  p.probe = &probe;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);

  int worst = 0, skips = 0;
  for (size_t i = 1; i < probe.cell.size(); ++i) {
    const int a = probe.cell[i - 1], b = probe.cell[i];
    const int dx = std::abs(a % p.res - b % p.res);
    const int dy = std::abs(a / p.res - b / p.res);
    const int step = std::max(dx, dy);  // Chebyshev: 1 == adjacent
    worst = std::max(worst, step);
    if (step > 1) ++skips;
  }
  // ENFORCED as of CFL-bounded displacement.
  Check("T4 particle never skips a cell", worst <= 1 && !probe.cell.empty(),
          F("%.0f of %.0f steps skipped; worst jump %.0f cells", double(skips),
            double(probe.cell.size()), double(worst)));
}

// --- T5. cascade threshold gate -------------------------------------------
// Calls Cascade DIRECTLY on a controlled neighbourhood rather than through a
// sim run. ReposeAngle already documents why a full run cannot assert this:
// the cascade is per-particle, so untouched cliff survives and the global
// maximum slope says nothing about the operator itself.
//
// Below repose it must be a no-op -- which is exactly the precondition the
// early-out optimisation relies on, so the optimisation and the invariant are
// the same test.
void CascadeThresholdGate() {
  Params p = Base(8);
  const float cell_m = p.world_m / float(p.res);
  const float max_diff =
      std::tan(p.repose_angle_deg * 3.14159265f / 180.0f) * cell_m / p.relief_m;
  auto build = [&](float angle_deg) {
    Grid g(p.res);
    std::fill(g.height.begin(), g.height.end(), 0.f);
    std::fill(g.soil.begin(), g.soil.end(), 1.0f);  // plenty of soil to move
    g.height[g.idx(4, 4)] =
        std::tan(angle_deg * 3.14159265f / 180.0f) * cell_m / p.relief_m;
    return g;
  };
  auto max_drop = [&](const Grid& g) {
    float m = 0.f;
    for (int k = 0; k < 8; ++k) {
      static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
      static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
      m = std::max(m, g.height[g.idx(4, 4)] - g.height[g.idx(4 + dx[k], 4 + dy[k])]);
    }
    return m;
  };

  Grid below = build(39.0f);
  const std::vector<float> before = below.height;
  Cascade(below, p, 4, 4, max_diff);
  const bool inert = (below.height == before);

  Grid above = build(41.5f);
  const float drop0 = max_drop(above);
  const float peak0 = above.height[above.idx(4, 4)];
  Cascade(above, p, 4, 4, max_diff);
  const float drop1 = max_drop(above);
  const float peak1 = above.height[above.idx(4, 4)];
  const bool relaxed = drop1 < drop0 && peak1 < peak0;

  Check("T5 cascade is inert below repose", inert,
        inert ? "39 deg: bit-identical" : "39 deg: MUTATED below repose");
  Check("T5 cascade relaxes above repose", relaxed,
        F("41.5 deg: drop %.4f -> %.4f, peak %+.4f", double(drop0),
          double(drop1), double(peak1 - peak0)));
}

// --- T6. mass conservation at production length ---------------------------
// The existing MassConservation covers a 200-step fixture and passes at 0.09%.
// The leak is CUMULATIVE (measured 200 -> 0.09%, 400 -> 0.14%, 1200 -> 0.41%,
// 3000 at 1024^2 -> 4.59%), so a short fixture cannot see it. This runs long
// enough to expose it and asserts a threshold a correct implementation clears
// by orders of magnitude -- 0.1%, not the 1% the short test uses.
void MassConservationLongRun() {
  Params p = Base(128);
  p.world_m = 16.0f * 128.0f;
  p.relief_m = 900.0f;
  p.steps = 1200;
  p.drops = 64;
  Grid g0(p.res);
  InitTerrain(g0, p);
  const double before = SumH(g0);
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  const double residual = (SumH(g) - before) + g.lost_offmap;
  const double rel = std::fabs(residual) / std::max(std::fabs(before), 1e-9);
  // ENFORCED. 0.410% -> 0.022% (concentration-as-mass) -> ~2e-9 (lake-branch
  // evaporation). The bound is 1e-5, not the 1e-3 this started at: conservation
  // is now exact to float noise, so anything looser would let a real leak back
  // in unnoticed. It still leaves ~4 orders of magnitude of headroom.
  Check("T6 mass conserves over a long run", rel < 1e-5,
        F("residual %.3e of %.3e (%.3f%%) over 1200 steps", residual, before,
          100 * rel));
}

// --- T7. the horseshoe drains to its outflow edge -------------------------
// The regional trend must fall monotonically toward the outflow edge. If it
// does not, some interior cell is a local minimum, priority-flood floods from
// the map border inward, and the whole map becomes one basin -- the failure
// InitBowl's comment records. Checked on the trend ALONE (noise amplitude
// zeroed), because the noise is supposed to add local pits; those become lakes
// and are the point. What must not exist is a REGIONAL sink.
void HorseshoeDrainsToOutflow() {
  Params p = Base(64);
  p.terrain = Params::Terrain::Horseshoe;
  p.amp_high_m = 0.f;  // trend only
  p.amp_low_m = 0.f;
  Grid g(p.res);
  InitTerrain(g, p);
  float worst_rise = -1e30f;
  for (int y = 0; y + 1 < g.n; ++y)
    for (int x = 0; x < g.n; ++x)
      worst_rise = std::max(worst_rise, g.height[g.idx(x, y + 1)] -
                                            g.height[g.idx(x, y)]);
  // Also assert the shape is actually a horseshoe: rim high, outflow low, and
  // the rim continuous across all three edges rather than only at the corners.
  const int mid = g.n / 2;
  const float rim_centre = g.height[g.idx(mid, 0)] * p.relief_m;
  const float rim_corner = g.height[g.idx(0, 0)] * p.relief_m;
  const float outflow = g.height[g.idx(mid, g.n - 1)] * p.relief_m;
  const bool shaped = rim_centre > outflow + 50.f &&
                      std::fabs(rim_centre - rim_corner) < 0.25f * rim_centre;
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "worst rise %+.2e m; rim centre %.0f m, corner %.0f m, "
                "outflow %.0f m",
                double(worst_rise) * p.relief_m, double(rim_centre),
                double(rim_corner), double(outflow));
  Check("T7 horseshoe drains to the outflow", worst_rise <= 0.f && shaped, buf);
}

// --- T8. the landscape does not depend on the integration step -------------
// THE acceptance test for the per-distance transport law, and the one the old
// law could never have passed.
//
// travel_step_m is pure numerics: how far the velocity ODE is advanced before
// the swept segment is traversed. Halving it doubles the iteration count and
// halves each segment. Under the reference's per-ITERATION relaxation that
// doubled the erosion outright -- which is exactly how changing the stride from
// sqrt(2) cells to 1 cell rewrote the landscape. Under a per-DISTANCE law the
// relaxation integrates along the path, so the answer must not move.
void StepSizeIndependence() {
  auto run = [](float step_m) {
    Params p = Base(48);
    p.terrain = Params::Terrain::Bowl;
    p.steps = 200;
    p.drops = 128;  // enough samples that Monte-Carlo noise sits under the bound
    p.travel_step_m = step_m;
    Grid g0(p.res);
    InitTerrain(g0, p);
    std::vector<Lake> lakes; SimStats st;
    Grid g = Run(p, lakes, st);
    double moved = 0.0;
    for (size_t i = 0; i < g.cells; ++i)
      moved += std::fabs(double(g.height[i]) - double(g0.height[i]));
    return moved / double(g.cells) * p.relief_m;  // mean |dh|, metres
  };
  const double coarse = run(16.f), mid = run(8.f), fine = run(4.f);
  auto rel = [](double a, double b) {
    return std::fabs(a - b) / std::max(std::fabs(a), 1e-9);
  };
  const double d_cm = rel(coarse, mid), d_mf = rel(mid, fine);
  char buf[200];
  std::snprintf(buf, sizeof(buf),
                "mean |dh| %.3f / %.3f / %.3f m at 16 / 8 / 4 m steps; "
                "successive %.0f%% then %.0f%%",
                coarse, mid, fine, 100 * d_cm, 100 * d_mf);
  Check("T8 landscape independent of step size", d_mf < 0.10, buf);
}

// --- T9. relaxation converges to the priority-flood surface ----------------
// The ORACLE for the whole water rewrite. PriorityFlood is exact, so it is kept
// as ground truth and the relaxation is checked against it rather than against
// itself. If these agree, swapping a global priority queue for a bounded grid
// pass is a change of METHOD, not of answer.
//
// Also measures how far a 4-sweep budget is from converged, since that is what
// runs per step in production -- an assertion is worth more than an assumption
// about "a few sweeps should do".
void WaterRelaxationMatchesFlood() {
  // 256, not 64: sweep propagation scales with grid size, so a small fixture
  // would report convergence the production grid does not have.
  Params p = Base(256);
  p.terrain = Params::Terrain::Horseshoe;
  Grid g(p.res);
  InitTerrain(g, p);

  std::vector<float> filled;
  std::vector<int32_t> outlet;
  PriorityFlood(g, filled, outlet);

  // Compared only where the oracle can SPEAK. PriorityFlood has no concept of
  // a sea: it fills to the map edge and stops. RelaxWater holds anything at or
  // below sea level AT sea level, so the two differ there by exactly the sea
  // depth -- 9.18 m measured, which is a difference of model, not an error.
  // Excluding those cells is not widening the bound; it is not asking a
  // question the oracle cannot answer.
  const float sea_hu_t = p.sea_level_m / p.relief_m;
  auto comparable = [&](int x, int y) {
    return x > 0 && y > 0 && x < g.n - 1 && y < g.n - 1 &&
           g.height[g.idx(x, y)] >= sea_hu_t;
  };
  RelaxWater(g, p, 400, true);  // cold, run to convergence
  double worst = 0.0, worst4 = 0.0;
  for (int y = 0; y < g.n; ++y)
    for (int x = 0; x < g.n; ++x)
      if (comparable(x, y))
        worst = std::max(worst, std::fabs(double(g.wsurf[g.idx(x, y)] -
                                                filled[g.idx(x, y)])));

  // And what a production-budget pass gets from cold.
  RelaxWater(g, p, p.water_sweeps, true);
  for (int y = 0; y < g.n; ++y)
    for (int x = 0; x < g.n; ++x)
      if (comparable(x, y))
        worst4 = std::max(worst4, std::fabs(double(g.wsurf[g.idx(x, y)] -
                                                  filled[g.idx(x, y)])));

  const double worst_m = worst * p.relief_m, worst4_m = worst4 * p.relief_m;
  char buf[200];
  std::snprintf(buf, sizeof(buf),
                "converged worst |dw| %.2e m; %d sweeps from cold %.2f m",
                worst_m, p.water_sweeps, worst4_m);
  // BOUNDED, not exact. The old 1e-3 m bound was only achievable because the
  // solver ran to convergence against a static field with no sea and no
  // relaxation over time. Water now relaxes as the terrain moves, so the
  // surface fluctuates by design and demanding exactness would be testing an
  // artefact of the old arrangement. 1 m against ~300 m of relief.
  Check("T9 water relaxation ~ priority flood", worst_m < 1.0, buf);
}

// --- T10. per-cell water, zero-loss limit == the flood surface -------------
// The limit test for the water field. With no loss and enough rain, every
// basin must fill to its spill and nowhere else -- which is exactly the
// priority-flood surface. So the emergent field is checked against an exact
// algorithm rather than against itself.
//
// It also pins the two things that are easy to get wrong and hard to see:
// water must not sit above a spill (over-fill), and must not pool on a slope
// (failure to drain).
void WaterFieldZeroLossLimit() {
  Params p = Base(64);
  p.terrain = Params::Terrain::Horseshoe;
  // No loss AND no rain: charge the map once and let it settle. This isolates
  // TRANSPORT, which is what the zero-loss limit is actually a claim about.
  // Driving it with rain instead conflates it with a rate balance -- at
  // dt_water = 2 yr each iteration adds 2 m of rain while levelling moves only
  // ~0.3 m downhill, so the map floods (measured 2248 m of over-fill) no matter
  // what the loss is. That rate constraint is real and calibrated separately.
  p.standing_water_loss_m_per_yr = 0.f;
  p.runoff_m_per_yr = 0.f;
  Grid g(p.res);
  InitTerrain(g, p);
  std::vector<float> flux(g.cells, 0.f);
  float hi = -1e30f, lo = 1e30f;
  for (float h : g.height) { hi = std::max(hi, h); lo = std::min(lo, h); }
  std::fill(g.water.begin(), g.water.end(), hi - lo);  // drown it
  UpdateWater(g, p, 4000, flux);

  std::vector<float> filled;
  std::vector<int32_t> outlet;
  PriorityFlood(g, filled, outlet);

  // Compare only where the flood says there IS a basin: elsewhere the flood
  // surface equals the terrain and both are trivially dry.
  double over = 0.0, under = 0.0;
  size_t basin = 0;
  for (size_t i = 0; i < g.cells; ++i) {
    if (filled[i] <= g.height[i] + 1e-7f) {
      over = std::max(over, double(g.water[i]));  // pooled on a slope?
      continue;
    }
    ++basin;
    const double want = double(filled[i] - g.height[i]);
    const double got = double(g.water[i]);
    over = std::max(over, got - want);
    under = std::max(under, want - got);
  }
  const double over_m = over * p.relief_m, under_m = under * p.relief_m;
  char buf[200];
  std::snprintf(buf, sizeof(buf),
                "%zu basin cells; worst over-fill %.2f m, worst under-fill "
                "%.2f m",
                basin, over_m, under_m);
  Check("T10 zero-loss water == flood surface", over_m < 0.5 && under_m < 0.5,
        buf);
}

// --- T11. channel orientation is isotropic ---------------------------------
// Guards against the grid biasing where water goes.
//
// |mean(exp(i*4*theta))| over cells carrying momentum -- the 4-fold Fourier
// component of the flow-direction distribution. A lattice bias clusters
// directions at 0/45/90 deg, which is exactly 4-fold, so this sits near the
// noise floor (~1/sqrt(N)) for an isotropic model and rises toward 1 as flow
// locks to the grid.
//
// It exists because the D8 snap it replaced was spotted BY EYE in a render.
// That is not a way to notice a bias coming back.
void ChannelOrientationIsotropy() {
  Params p = Base(128);
  p.terrain = Params::Terrain::Horseshoe;
  p.world_m = 16.0f * 128.0f;
  p.steps = 400;
  p.drops = 128;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  double sr = 0, si = 0;
  size_t n = 0;
  for (size_t i = 0; i < g.cells; ++i) {
    const float mx = g.momx[i], my = g.momy[i];
    if (std::sqrt(mx * mx + my * my) < 1e-6f) continue;
    const double th = std::atan2(double(my), double(mx));
    sr += std::cos(4.0 * th);
    si += std::sin(4.0 * th);
    ++n;
  }
  const double a4 = n ? std::sqrt(sr * sr + si * si) / double(n) : 0.0;
  Check("T11 channel orientation is isotropic", a4 < 0.15,
        F("4-fold anisotropy %.3f over %.0f cells (noise floor %.3f)", a4,
          double(n), n ? 1.0 / std::sqrt(double(n)) : 1.0));
}

// --- T12. evaporation reaches a real equilibrium -----------------------------
// Fills a basin from DRY and checks where it settles.
//
// Equilibrium is an area balance: loss * lake_area = runoff * catchment_area,
// so the lake should stabilise covering runoff/loss of the map. Nothing tested
// this before -- LakeFillRate looks like it does but sets evaporation to ZERO
// and asserts a capacity bound, so the balance itself was never exercised.
//
// Erosion, cascade and lake deposition are off: this is the water budget alone.
void EvaporationEquilibrium() {
  Params p = Base(64);
  p.terrain = Params::Terrain::Bowl;
  // Basin must sit ABOVE sea level: sea_level_m = 0 pins anything at or below
  // zero as a drain, and the default bowl's floor is at -300 m, so the whole
  // basin was being treated as ocean and forced dry.
  p.bowl_rim_m = 900.0f;
  p.bowl_well_m = 300.0f;
  p.prefill = false;        // must FILL to equilibrium, not start at it
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.enable_lake_deposit = false;
  p.runoff_m_per_yr = 1.0f;
  p.evaporation_m_per_yr = 20.0f;   // -> equilibrium area = 1/20 = 5% of the map
  p.steps = 800;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  size_t wet = 0;
  for (float w : g.water) if (w > 0.f) ++wet;
  const double got = double(wet) / double(g.cells);
  const double want = double(p.runoff_m_per_yr) / double(p.evaporation_m_per_yr);
  const double err = std::fabs(got - want) / want;
  Check("T12 evaporation reaches equilibrium", err < 0.30,
        F("wet %.2f%% vs predicted %.2f%% (runoff/loss); %.0f%% off", 100 * got,
          100 * want, 100 * err));
}

// --- T13. a basin below sea level holds water, it does not drain -------------
// Sea level is a FLOOR on the water surface: it can only ever add water, never
// remove it. A hole whose floor is below sea level is an arm of the sea and
// must sit full to sea level.
//
// RED at the time of writing: the boundary was applied to DEPTH (water = 0)
// instead of to the SURFACE (water = sea_level - height), so sub-sea-level
// ground was emptied every iteration instead of filled.
void SeaLevelFillsNotDrains() {
  Params p = Base(32);
  p.sea_level_m = 0.0f;
  p.evaporation_m_per_yr = 0.0f;   // isolate the boundary condition
  Grid g(p.res);
  // Plateau at +100 m with a hole down to -50 m in the middle.
  std::fill(g.height.begin(), g.height.end(), 100.0f / p.relief_m);
  for (int y = 14; y <= 17; ++y)
    for (int x = 14; x <= 17; ++x) g.height[g.idx(x, y)] = -50.0f / p.relief_m;
  // Start it correctly full: surface exactly at sea level.
  for (int y = 14; y <= 17; ++y)
    for (int x = 14; x <= 17; ++x)
      g.water[g.idx(x, y)] = 0.0f / p.relief_m - g.height[g.idx(x, y)];
  std::vector<float> flux(g.cells, 0.f);
  UpdateWater(g, p, 50, flux);

  float worst_surf = 0.f, min_depth = 1e30f;
  for (int y = 14; y <= 17; ++y)
    for (int x = 14; x <= 17; ++x) {
      const size_t i = g.idx(x, y);
      const float surf_m = (g.height[i] + g.water[i]) * p.relief_m;
      worst_surf = std::max(worst_surf, std::fabs(surf_m - p.sea_level_m));
      min_depth = std::min(min_depth, g.water[i] * p.relief_m);
    }
  Check("T13 sub-sea basin fills, does not drain",
        min_depth > 45.f && worst_surf < 1.0f,
        F("min depth %.1f m (expect ~50), worst surface offset %.2f m",
          double(min_depth), double(worst_surf)));
}

// --- T14. the two boundary conditions are OPPOSITE ---------------------------
// They were collapsed into one predicate, which is how the sign error hid:
//   map edge    -- OUTFLOW. Water leaves. Depth goes to 0.
//   below sea   -- SEA.     Surface held AT sea level. Depth = sea - height.
// Same treatment for both is wrong for one of them.
void WaterBoundaryConditions() {
  Params p = Base(32);
  p.sea_level_m = 0.0f;
  p.evaporation_m_per_yr = 0.0f;
  Grid g(p.res);
  std::fill(g.height.begin(), g.height.end(), 100.0f / p.relief_m);
  // A sub-sea shelf on the left interior, dry to begin with.
  for (int y = 8; y <= 23; ++y)
    for (int x = 2; x <= 6; ++x) g.height[g.idx(x, y)] = -30.0f / p.relief_m;
  // Water dumped on a map-edge cell: it must leave.
  g.water[g.idx(0, 16)] = 20.0f / p.relief_m;
  std::vector<float> flux(g.cells, 0.f);
  UpdateWater(g, p, 80, flux);

  const float edge_depth = g.water[g.idx(0, 16)] * p.relief_m;
  float shelf_worst = 0.f;
  for (int y = 10; y <= 21; ++y)
    for (int x = 3; x <= 5; ++x) {
      const size_t i = g.idx(x, y);
      shelf_worst = std::max(shelf_worst,
                             std::fabs((g.height[i] + g.water[i]) * p.relief_m -
                                       p.sea_level_m));
    }
  Check("T14 outflow drains, sea fills",
        edge_depth < 0.5f && shelf_worst < 1.0f,
        F("map-edge depth %.2f m (expect 0), sea-shelf surface off by %.2f m "
          "(expect 0)", double(edge_depth), double(shelf_worst)));
}

// --- T15. water fluctuates, it does not accumulate ---------------------------
// The failure this guards against is silent: a small per-step surplus that
// never drains looks fine early and floods the map late, so a single end-state
// snapshot cannot see it. Only the TREND can.
//
// Samples total stored water at increasing run lengths. The seed is fixed and
// the sim deterministic, so a run of N steps IS the trajectory at time N.
// Asserts the late samples do not trend upward and stay within a band.
void WaterDoesNotAccumulate() {
  auto stored_m3 = [](int steps) {
    Params p = Base(64);
    p.terrain = Params::Terrain::Bowl;
    p.bowl_rim_m = 900.0f;
    p.bowl_well_m = 300.0f;
    p.enable_erosion = false;   // isolate the water budget from the bed moving
    p.enable_cascade = false;
    p.evaporation_m_per_yr = 20.0f;
    p.steps = steps;
    std::vector<Lake> lakes; SimStats st;
    Grid g = Run(p, lakes, st);
    const double cell_area = double(p.world_m / p.res) * double(p.world_m / p.res);
    double v = 0;
    for (float w : g.water) v += double(w);
    return v * double(p.relief_m) * cell_area;
  };
  const int at[5] = {200, 400, 600, 800, 1000};
  double v[5];
  for (int i = 0; i < 5; ++i) v[i] = stored_m3(at[i]);

  // Trend over the LATE samples: equilibrium means the mean stops moving.
  const double early = (v[1] + v[2]) * 0.5, late = (v[3] + v[4]) * 0.5;
  const double growth = (early > 0) ? (late - early) / early : 0.0;
  // Band: how far the late samples stray from their own mean.
  double lo = v[2], hi = v[2];
  for (int i = 2; i < 5; ++i) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
  const double mean = (v[2] + v[3] + v[4]) / 3.0;
  const double band = (mean > 0) ? (hi - lo) / mean : 0.0;

  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "%.2e -> %.2e m3 at 200..1000 steps; late growth %+.1f%%, "
                "band %.1f%%",
                v[0], v[4], 100 * growth, 100 * band);
  Check("T15 water fluctuates, does not accumulate",
        growth < 0.10 && band < 0.35, buf);
}

int RunAll() {
  std::printf("protogen sanity tests (small grids, production 16 m cells)\n");
  MassConservation();
  FlatPlaneInert();
  Determinism();
  KnobLiveness();
  LakeFlatness();
  LakeIdValid();
  LakeAreaBounded();
  DischargeMatchesArea();
  ValleyChannel();
  ReposeAngle();
  LobeErodes();
  BedrockResists();
  SubstrateInertWhileSoilLasts();
  SoilStaysPhysical();
  DepositionBuildsSoil();
  Symmetry();
  ResolutionIndependence();
  DeltaProfile();

  std::printf("\n  physics invariants (PEND = mechanism not built yet)\n");
  TerminalVelocityManning();
  VolumeDiscretizationInvariance();
  DiffusionRelaxesRidge();
  ParticleNeverSkipsACell();
  CascadeThresholdGate();
  MassConservationLongRun();
  HorseshoeDrainsToOutflow();
  StepSizeIndependence();
  WaterRelaxationMatchesFlood();
  WaterFieldZeroLossLimit();
  ChannelOrientationIsotropy();
  EvaporationEquilibrium();
  SeaLevelFillsNotDrains();
  WaterBoundaryConditions();
  WaterDoesNotAccumulate();

  std::printf("\n  %d passed, %d failed, %d pending", g_pass, g_fail, g_pending);
  if (g_pending_ready)
    std::printf(" (%d of them now PASS -- promote to Check())", g_pending_ready);
  std::printf("\n");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace test

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--test") return test::RunAll();
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) != "--extract-rivers") continue;
    if (i + 1 >= argc) {
      std::fprintf(stderr, "protogen: --extract-rivers needs a directory\n");
      return 2;
    }
    return RunExtractRivers(argv[i + 1]);
  }
  Params p;
  try {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto nxt = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--seed") p.seed = uint32_t(std::stoul(nxt()));
    else if (a == "--res") p.res = std::stoi(nxt());
    else if (a == "--world") p.world_m = std::stof(nxt());
    else if (a == "--relief") p.relief_m = std::stof(nxt());
    else if (a == "--steps") p.steps = std::stoi(nxt());
    else if (a == "--drops") p.drops = std::stoi(nxt());
    else if (a == "--runoff") p.runoff_m_per_yr = std::stof(nxt());
    else if (a == "--evaporation") p.evaporation_m_per_yr = std::stof(nxt());
    else if (a == "--grain-velocity")
      p.settling_velocity_m_per_s = std::stof(nxt());
    else if (a == "--plume-velocity") p.plume_velocity_m_per_s = std::stof(nxt());
    else if (a == "--lake-interval") p.lake_interval = std::stoi(nxt());
    else if (a == "--min-lake-area") p.min_lake_area_m2 = std::stof(nxt());
    else if (a == "--min-lake-depth") p.min_lake_depth_m = std::stof(nxt());
    else if (a == "--entrainment") p.entrainment = std::stof(nxt());
    else if (a == "--adaptation-length")
      p.adaptation_length_m = std::stof(nxt());
    else if (a == "--travel-step") p.travel_step_m = std::stof(nxt());
    else if (a == "--momentum") p.momentum_transfer = std::stof(nxt());
    else if (a == "--sea-level") p.sea_level_m = std::stof(nxt());
    else if (a == "--amp-low") p.amp_low_m = std::stof(nxt());
    else if (a == "--lrate") p.lrate = std::stof(nxt());
    else if (a == "--repose") p.repose_angle_deg = std::stof(nxt());
    else if (a == "--bedrock-erodibility") p.bedrock_erodibility = std::stof(nxt());
    else if (a == "--initial-soil") p.initial_soil_m = std::stof(nxt());
    else if (a == "--cascade-settling") p.settling = std::stof(nxt());
    else if (a == "--snapshot-every") p.snapshot_every = std::stoi(nxt());
    else if (a == "--bowl") p.bowl = true;
    // The pre-horseshoe substrate, for A/B against the audit baseline.
    else if (a == "--fbm") p.terrain = Params::Terrain::Noise;
    // Soil creep coefficient, m2/yr. Non-zero enables the pass.
    else if (a == "--diffusion") {
      p.diffusion_D_m2_per_yr = std::stof(nxt());
      p.enable_diffusion = p.diffusion_D_m2_per_yr > 0.f;
    }
    else if (a == "--tilt") p.tilt_m = std::stof(nxt());
    else if (a == "--rim") p.rim_m = std::stof(nxt());
    else if (a == "--trough") p.trough_m = std::stof(nxt());
    else if (a == "--no-disperse") p.disperse = false;
    else if (a == "--prefill") p.prefill = true;
    else if (a == "--wander") p.plume_wander_deg = std::stof(nxt());
    else if (a == "--dt") p.dt_years = std::stof(nxt());
    else if (a == "--min-dispersion-depth") p.min_dispersion_depth_m = std::stof(nxt());
    else if (a == "--source-x") p.source_x_frac = std::stof(nxt());
    else if (a == "--source-y") p.source_y_frac = std::stof(nxt());
    else if (a == "--source-jitter") p.source_jitter_cells = std::stof(nxt());
    else if (a == "--bowl-rim") p.bowl_rim_m = std::stof(nxt());
    else if (a == "--bowl-well") p.bowl_well_m = std::stof(nxt());
    else if (a == "--out") p.out = nxt();
    else { std::fprintf(stderr, "protogen: unknown arg '%s'\n", a.c_str()); return 2; }
  }
  } catch (const std::exception& e) {
    // A trailing flag makes nxt() return "", and stof("") throws. Without this
    // `protogen --steps` aborted instead of reporting the problem.
    std::fprintf(stderr, "protogen: bad or missing argument value (%s)\n", e.what());
    return 2;
  }

  const float cell_m = p.world_m / float(p.res);
  // Each particle stands for the runoff over its share of the map, which is how
  // the dimensionless volume track converts to a real discharge.
  const double q_per_unit_vol_m3_s =
      double(p.runoff_m_per_yr) * double(p.world_m) * double(p.world_m) /
      double(p.drops) / kSecondsPerYear;

  Grid g(p.res);
  InitTerrain(g, p);

  const unsigned workers = std::max(1u, badlands::GetWorkerThreadCount());

  std::printf("protogen: %dx%d cells, %.0f m world, %.1f m cells, %d workers\n"
              "  relief %.0f m, %d steps x %d drops\n"
              "  runoff %.2f m/yr, evaporation %.2f m/yr, w_s %.1e m/s\n",
              p.res, p.res, p.world_m, cell_m, workers, p.relief_m, p.steps,
              p.drops, p.runoff_m_per_yr, p.evaporation_m_per_yr,
              p.settling_velocity_m_per_s);

  {
    std::error_code ec;
    std::filesystem::create_directories(p.out, ec);
    if (ec) {
      std::fprintf(stderr, "protogen: cannot create out dir '%s': %s\n",
                   p.out.c_str(), ec.message().c_str());
      return 1;
    }
  }
  Dump(g, p, "0000-initial");


  std::vector<Lake> lakes;
  SimStats st;
  RunSim(p, g, lakes, st, true);
  std::printf("protogen: done\n  timings (s): drops %.1f grid %.1f lakes %.1f\n",
              st.t_drops, st.t_grid, st.t_lake);

  // Output boundary: world.txt + rivers.bin, off the FINISHED grid. Same
  // function --extract-rivers calls, so the two paths cannot diverge.
  if (!WriteWorldArtifacts(BuildInputsFromGrid(g, p), p.out)) return 1;
  return 0;
}
