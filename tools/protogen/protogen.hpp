#pragma once

// The shared surface between protogen's TUs (protogen.cpp: phase-0 + driver;
// protogen_swe.cpp: the phase-1 SWE fluid passes; protogen_tests.cpp: the
// sanity-test suite): the grid/parameter types every phase operates on, plus
// the handful of functions reachable from more than one TU. Everything else
// stays internal (an anonymous namespace) to the TU that defines it -- see
// each .cpp's own comments for what that is. Export deliberately: this file
// grows only when a symbol gains an actual caller in another TU.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pg {

constexpr double kSecondsPerYear = 31557600.0;

// SWE velocity bound, as a multiple of the local shallow-water wave speed
// sqrt(g*h). NOT a physical claim (real open-channel flows rarely exceed
// Froude ~2-3 even at spillways) -- it exists purely as a numerical backstop
// where a virtual pipe's carried-over flux (momentum from a PREVIOUS,
// possibly deeper substep) gets reinterpreted through a since-collapsed
// A_pipe (built from the CURRENT, shallower depth): a depth that fell
// faster than the flux drained inflates the implied velocity without bound
// (measured 113 m/s / Froude ~75 in 0.2 m water on the wet-dry cliff fixture
// before this bound existed). 10x is an order of magnitude above any
// legitimate steep-channel Froude number, so it never clips normal
// supercritical dynamics, but 10x below the ~75 the bug produced -- closes
// the runaway without becoming a de facto flow limiter. Shared here (not a
// private constant in protogen_swe.cpp) so SweWetDryFrontStability can
// assert against the SAME number the implementation enforces, not a
// hand-copied duplicate that could silently drift out of sync.
constexpr float kMaxFroude = 10.0f;
// MORFAC clamp scaffolding (Task 6 activates this, Task 4 only seeds it --
// see Params::morfac and ClampMorfacBedDelta below): the fraction of a
// cell's water depth a single fluid cycle's morpho hook may move the bed by,
// AFTER morfac scaling. 10%: a starting bound with no bed-delta magnitude
// yet to calibrate it against (nothing calls ClampMorfacBedDelta today), so
// it may need retuning once Task 6 supplies a real erosion/deposition term
// -- the number exists so the clamp is never silently unbounded, not because
// 10% is a derived physical limit.
constexpr float kMaxBedDeltaFraction = 0.1f;

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

  // --- suspended sediment settling ---
  //
  // The particle walk's terminal exit (Descend's post-loop comment, "Reached
  // max_age still carrying a load") used to dump its whole remaining load
  // into the ONE cell it died in -- measured at ~40% of all deposited mass,
  // and the likely source of thousands of one-cell pits. It now injects into
  // `sus` (Grid::sus) instead, and this is how that field drains: each
  // landscape step, `settle_fraction` of a cell's suspended load joins the
  // bed as soil (SettleSus, mirroring Deposit), and `sus_diffusion` of what
  // is left spreads equally to the four orthogonal neighbours before the next
  // step's settling gets a turn at it.
  //
  // 0.1: a terminal load is ~96% settled by 30 steps ((1-0.1)^30 = 4.2%) and
  // ~99.5% by 50 -- "tens of steps", not instant (which would just move the
  // one-cell spike, not remove it) and not so slow it never resolves within a
  // production run's step count.
  float settle_fraction = 0.1f;
  // 0.2: modest relative to settle_fraction, so most of a load still settles
  // near where it landed (the fix concentrates the fill near the death cell,
  // it does not erase it) while enough spreads each step to smear the old
  // one-cell spike into a shallow halo a few cells wide instead.
  float sus_diffusion = 0.2f;

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
  // Temperate default. `runoff_m_per_yr` feeds BOTH phase-0 (EffectiveDropVolume
  // via the particle count) and phase-1 (SweDepth's rain term, same real rate).
  // `evaporation_m_per_yr` regains a consumer as of the SWE fluid passes:
  // SweDepth subtracts it from wet cells only, clamped so depth cannot go
  // negative. It has no phase-0 effect (the particle walk's own evaporation is
  // `evap_length_m`, a travel-distance e-fold, not this real-rate knob).
  float runoff_m_per_yr = 1.0f;
  float evaporation_m_per_yr = 0.8f;

  // Regime width w = k_w*sqrt(Q), reused from the repo's erosion.hpp -- sets
  // the width term in the Manning flow-depth closure Descend uses off-channel
  // (see the depth_m derivation there).
  float channel_width_coeff = 5.0f;

  // --- phase-1 SWE fluid: virtual-pipes shallow water (protogen_swe.cpp) ---
  //
  // Substeps run per RunSweCycles cycle, each Flux->Depth->Velocity. 50 is a
  // starting point, not derived: the substep's OWN dt already comes from CFL,
  // this just says how many of them run before the (currently empty) morpho
  // hook gets a turn once Task 6 fills it in.
  int swe_substeps = 50;
  // CFL number for the substep dt: dt = cfl_number * cell_m / sqrt(g*h_max).
  // 0.5 is the standard shallow-water safety factor -- explicit virtual-pipes
  // schemes are stable up to ~1.0; 0.5 leaves headroom for the implicit drag
  // term's own stiffness and for MORFAC-scaled bed change later.
  float cfl_number = 0.5f;
  // Manning roughness for the SWE fluid passes. A SEPARATE knob from
  // `manning_n` (the phase-0 particle walk's), not an alias, even though the
  // drag physics is identical (g*n^2/depth^(4/3)): the two closures see
  // different flow regimes -- phase-0 is thin sheet-flow floored at
  // `sheet_flow_depth_m`, phase-1 is a settled depth field spanning
  // millimetres to lake-deep -- so retuning one must not silently retune the
  // other. Same default (0.035) because nothing yet says the terrains
  // disagree; a future task can diverge them once one does.
  float swe_manning_n = 0.035f;
  // Wet/dry threshold, metres. Below this a cell's OUTFLOW is forced to
  // exactly zero regardless of head difference or carried-over flux, rather
  // than left to fade out via the depth factor already in the flux law --
  // so a nearly-dry cell cannot keep exporting on inertia from a wetter past,
  // and the velocity derivation has a hard floor to guard against. 1 mm:
  // shallower than any depth this sim treats as meaningful, deep enough to
  // clear ordinary float noise in `h`.
  float eps_wet = 1e-3f;
  // Abort floor for the CFL-derived substep dt, seconds. Dt collapsing below
  // this means either the CFL max-reduction found something unphysical
  // (about to divide by ~0 wave speed) or a caller pushed cfl_number/Manning
  // to an absurd combination -- either way, continuing would spend cycles
  // producing output with no diagnosable meaning instead of naming the fault.
  float dt_floor_s = 1e-5f;
  // Morphological acceleration factor: how many fluid cycles' worth of bed
  // change Task 6's morpho hook is allowed to apply per ACTUAL fluid cycle
  // (the standard MORFAC trick -- run the fluid at its own timescale, but
  // let the slower bed evolve faster than real sediment transport would, so
  // a landform-scale run does not need landform-scale fluid time). 1.0
  // (identity, i.e. no acceleration) because nothing computes a bed delta
  // yet to accelerate -- Task 6 is this knob's first real consumer; see
  // `ClampMorfacBedDelta` below for where it is applied.
  float morfac = 1.0f;

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
  // Suspended sediment awaiting settlement, height units per cell -- what the
  // particle walk's terminal exit injects instead of dumping straight onto
  // the bed (see Params::settle_fraction). Ping-pong like the other grid
  // fields SettleSus updates via a Jacobi pass.
  std::vector<float> sus, sus_b;
  std::vector<float> vol_track, mx_track, my_track;
  std::vector<uint32_t> visits;
  // Mass balance, in height units x cells (see the residual print).
  // `injected_sus` counts what the terminal exit puts into `sus` (it used to
  // be `deposited_death`, counting a direct bed deposit at the same site).
  // `lost_offmap` also gains SettleSus's border-diffusion loss -- the same
  // ledger a particle walking off the map pays into.
  double injected_sus = 0.0, lost_offmap = 0.0;

  // --- phase-1 SWE fluid state (protogen_swe.cpp) ---
  //
  // Depth `h`/`h_b`, metres, ping-ponged like every other Jacobi field here.
  // `flux` needs NO ping-pong: SweFlux only ever reads a cell's own PREVIOUS
  // flux plus depth (`h`, which SweFlux never writes), so overwriting the one
  // buffer in place is never read-after-write hazardous -- see SweFlux's own
  // comment for the full walk. Face order matches Diffuse/SettleSus's
  // dx4/dy4: 0 = +x, 1 = -x, 2 = +y, 3 = -y; every entry is an OUTFLOW
  // (>= 0), never signed -- the neighbour's own opposite-face entry is how a
  // cell's inflow is read.
  std::vector<float> h, h_b;
  std::vector<std::array<float, 4>> flux;
  // Cell-centred velocity, m/s -- derived each substep from `flux`/`h` alone
  // (SweVelocity), not simulated state of its own.
  std::vector<float> velx, vely;
  // Water ledger, cubic metres. Rain in minus evaporation out minus what the
  // open border drained away must equal the change in Sigma(h)*cell_area
  // (SweWaterLedger). m^3 rather than a depth: every term is already a
  // dt*rate*area or dt*flux integral at the point the pass touches the cell,
  // so a further division back to a depth would only discard precision for
  // no benefit -- there is no single area to divide a MULTI-CELL sum by.
  double swe_rain_in_m3 = 0.0, swe_evap_out_m3 = 0.0,
         swe_border_outflow_m3 = 0.0;

  explicit Grid(int res)
      : n(res), cells(size_t(res) * res), height(cells, 0.f),
        height_b(cells, 0.f),
        vol_ema(cells, 0.f), vol_ema_b(cells, 0.f),
        discharge(cells, 0.f), discharge_b(cells, 0.f),
        Qm3s(cells, 0.f), Qm3s_b(cells, 0.f),
        momx(cells, 0.f), momy(cells, 0.f), momx_b(cells, 0.f), momy_b(cells, 0.f),
        soil(cells, 0.f), sus(cells, 0.f), sus_b(cells, 0.f),
        vol_track(cells, 0.f), mx_track(cells, 0.f), my_track(cells, 0.f),
        visits(cells, 0u),
        h(cells, 0.f), h_b(cells, 0.f),
        flux(cells, std::array<float, 4>{0.f, 0.f, 0.f, 0.f}),
        velx(cells, 0.f), vely(cells, 0.f) {}

  inline size_t idx(int x, int y) const { return size_t(y) * n + x; }
  inline bool oob(int x, int y) const {
    return x < 1 || y < 1 || x >= n - 1 || y >= n - 1;
  }
};

// Extracted from main so the sanity tests exercise the REAL loop rather than
// a copy that can drift from it.
struct SimStats {
  double t_drops = 0, t_grid = 0, t_settle = 0;
  // Phase-1 SWE cycle timers, one bucket per dispatch-list pass plus the
  // per-cycle CFL/tripwire reduction. Unused (stays zero) until RunSweCycles
  // is wired into the driver (Task 7); RunSweCycles itself already fills
  // them in when given a non-null SimStats*, so a caller can time it today.
  double t_swe_reduce = 0, t_swe_flux = 0, t_swe_depth = 0, t_swe_velocity = 0;
};

// Definitions in protogen.cpp -- phase-0 land init, the whole-sim driver, and
// the repose-angle cascade the test suite exercises directly (CascadeThresholdGate).
void InitTerrain(Grid& g, const Params& p);
void RunSim(const Params& p, Grid& g, SimStats& st, bool verbose);
void Cascade(Grid& g, const Params& p, int x, int y, float max_diff);

// Barnes et al. priority-flood: every cell raised to the lowest elevation on
// any path to the map edge, i.e. its spill level. Defined in protogen.cpp.
// Test oracle only (SweFillOracle here; Task 5's warm start reuses it for
// real) -- no production caller, exported because it now has one across a
// TU boundary (see this file's header comment on exporting deliberately).
void PriorityFlood(const Grid& g, std::vector<float>& filled,
                   std::vector<int32_t>& outlet_of);

// Phase-1 SWE fluid passes (protogen_swe.cpp). Each is a standalone,
// gather-only Jacobi pass over the whole grid, reading only front-buffer
// state and its own previous flux -- see protogen_swe.cpp's header comment
// for the pass list's role as the frozen future GPU dispatch order, and each
// function's own comment for its exact read/write set.
void SweFlux(Grid& g, const Params& p, float dt);
void SweDepth(Grid& g, const Params& p, float dt);
void SweVelocity(Grid& g, const Params& p);

// The phase-0 -> phase-1 handoff (Task 5). ONE-SHOT plain CPU code, run
// EXACTLY ONCE between phase-0's finished bed and phase-1's first
// RunSweCycles call -- NOT a Jacobi pass, and deliberately NOT part of the
// SweFlux/SweDepth/SweVelocity dispatch list above (that list is the frozen
// future GPU dispatch order; this runs once on the CPU, off that list, by
// design). Starting phase 1 bone dry means waiting on real rain
// (`runoff_m_per_yr`, ~1 m/yr) to fill even a modest lake from nothing, which
// is on the order of 1e8 substeps; this instead seeds `h` near the true
// steady state -- lakes prefilled to their spill level, channels prefilled to
// a Manning normal depth -- so a production run's fluid cycles MAINTAIN the
// water surface rather than spend nearly all of it filling one. See
// protogen_swe.cpp for the two-step derivation.
void SweWarmStart(Grid& g, const Params& p);

// Outcome of RunSweCycles: `ok` false means a tripwire fired -- non-finite
// state or a CFL-derived dt collapsed below `dt_floor_s`. `aborted_cycle`/
// `reason` name where and why, so a caller (test or, from Task 7, the
// driver) can report it rather than merely detect it.
struct SweRunResult {
  bool ok = true;
  int aborted_cycle = -1;
  std::string reason;
};
// Runs `cycles` SWE cycles (CFL dt -> swe_substeps x Flux/Depth/Velocity ->
// empty morpho hook) starting from Grid `g`'s current `h`/`flux` state --
// callable standalone, independent of RunSim/phase-0 (Task 5 supplies the
// warm start that seeds `h` from phase-0's finished bed; nothing here
// requires it). `stats`, if non-null, accumulates per-pass wall time.
SweRunResult RunSweCycles(Grid& g, const Params& p, int cycles,
                          SimStats* stats = nullptr);

// MORFAC clamp scaffolding for Task 6's morpho hook (SedExchange et al.):
// scales a proposed bed-elevation delta (metres, either sign) by
// `p.morfac`, then bounds the magnitude to `kMaxBedDeltaFraction * depth_m`
// so one fluid cycle's accelerated bed change can never exceed a fixed
// fraction of the water sitting on top of it. INERT today -- nothing calls
// this yet (the morpho hook is empty, see RunSweCycles's comment) -- so
// Task 6 ACTIVATES this path by calling it, rather than inventing it from
// scratch. Exported (not a private helper in protogen_swe.cpp) so a test
// can exercise its clamp behaviour directly.
float ClampMorfacBedDelta(float raw_delta_m, float depth_m, const Params& p);

}  // namespace pg

// The test entry point. Defined in protogen_tests.cpp; called from main().
namespace test {
int RunAll();
}
