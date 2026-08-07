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
// MORFAC clamp bound (ACTIVE as of Task 6 -- SedExchange is its caller; see
// Params::morfac and ClampMorfacBedDelta below): the fraction of a cell's
// water depth a single fluid cycle's morpho hook may move the bed by, AFTER
// morfac scaling.
//
// 10%, and Task 6 KEPT the number after calibrating it against a real bed
// delta rather than inheriting it. The bound is what makes MORFAC legitimate
// at all: MORFAC's whole premise is that the flow field computed this cycle
// stays VALID for the M cycles of bed change it is being credited with, and
// that premise dies as soon as the bed moves a significant fraction of the
// water depth -- the head field the next cycle's SweFlux solves is then a
// different problem from the one the exchange was computed on. 10% of depth
// means the flow the exchange saw is still ~90% correct when the bed is
// updated, i.e. the quasi-steady assumption is bent, not broken.
//
// Two measured checks that the number is neither too tight nor too loose:
//   - too tight would starve the physics: at the production capacity scale
//     (Params::capacity_Kc_s, C ~ 1e-3 m of sediment in an active channel)
//     one cycle's UNACCELERATED bed delta is ~5e-4 m, which is 0.5% of a
//     0.1 m deep channel -- 20x under the bound, so the clamp never touches
//     ordinary transport and only engages once MORFAC is genuinely large.
//   - too loose would let MORFAC drill: GeologyNoBottomlessPits drives
//     morfac = 10,000 at a local velocity spike, where the unclamped bed
//     delta is metres per cycle. The bound is exactly what converts that
//     into a bounded incision (see that test).
constexpr float kMaxBedDeltaFraction = 0.1f;
// SedAdvect step control. A semi-Lagrangian backtrace assumes the trajectory
// over its step is a STRAIGHT line at the cell's own velocity, which is only
// true while the parcel stays in a locally-uniform part of the field; a long
// backtrace through a curving channel cuts the corner and lands the sample
// somewhere the water never went. And the cycle's advective step is genuinely
// long: `swe_substeps` fluid substeps at CFL 0.5 move the water roughly
// swe_substeps*0.5*Froude cells, which on a measured fixture came to ~6 cells
// per cycle -- not a perturbation, a traverse.
//
// So the pass SUB-STEPS: it takes ceil(max displacement in cells) backtraces
// of `dt_morpho`/that, capped at kMaxAdvectSubsteps, so each one moves about
// a cell. Each sub-step re-gathers, which means the composite path follows
// the velocity field piecewise instead of shooting a single straight chord --
// the same reason a multi-step ODE integrator beats one Euler leap.
//
// 8 sub-steps: covers the ~6-cell traverse measured at the production substep
// count with headroom, and bounds the cost at 8 gather passes against the
// 3*swe_substeps the fluid already spends. kMaxBacktraceCells then only binds
// when the cap has already been hit (a single anomalously fast cell, e.g. one
// riding the Froude clamp), and 4 cells keeps even that sample inside a
// neighbourhood the Catmull-Rom stencil spans a couple of times over.
// Both are shared here so MorphoAdvectStepControl asserts against the SAME
// numbers the pass enforces.
constexpr int kMaxAdvectSubsteps = 8;
constexpr float kMaxBacktraceCells = 4.0f;
// Bound on SedAdvect's mass fixer, as a multiplicative factor either way.
// The fixer rescales the suspended field to a total the pass computes
// independently, which is fine while the correction is small and dangerous
// when it is not: an unbounded `target/after` lets a collapsing gather
// multiply the whole field arbitrarily, and since the fixer sets the total
// the mass audit then checks, the audit is structurally blind to it. 2x is
// well above anything a healthy step produces (measured worst case 1.31x on
// a warm-started fixture, and only in the first two cycles) and well below
// the 3x a bone-dry start produced before the bound existed. Beyond it the
// pass stops scaling and books the difference to
// `Grid::swe_sed_advect_fix_residual_m3` instead.
constexpr double kMaxAdvectFixFactor = 2.0;

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

  // --- phase-1 morphodynamics: the Exner exchange (protogen_swe.cpp) -------
  //
  // Transport capacity, MEI FORM ON THE ENERGY SLOPE:
  //     C = capacity_Kc_s * |grad(bed + h)| * |v|
  // with C an EQUIVALENT SEDIMENT DEPTH in metres (the depth of solid the
  // water column over this cell can hold in suspension) -- so
  // capacity_Kc_s carries units of SECONDS: [m] = [s] * [1] * [m/s].
  //
  // Both factors vanish in a still lake (flat water surface -> zero energy
  // slope; no flow -> zero speed), which is the property StillLakeInert
  // pins. The slope is the ENERGY slope, not the bed slope, which is what
  // keeps C > 0 for flowing water over a locally FLAT bed (the water surface
  // still tilts -- that tilt is what drives the flow) and is what stops a
  // flat reach terracing; FlatReachTransport pins that.
  //
  // 0.1 s, from one reference point rather than a fit: an active mountain
  // channel at S = 0.01, |v| = 1 m/s gives C = 1e-3 m of sediment over a
  // ~0.5 m deep flow, i.e. a volumetric concentration of 2e-3 (~5 g/l) --
  // turbid, but squarely inside the 0.1-10 g/l range measured in real
  // sediment-carrying rivers. This is the one genuinely free knob in the
  // morpho set (it sets the absolute transport RATE, which no other quantity
  // here pins), so production tuning of the landform timescale belongs on
  // this knob and on `morfac`, not on the physics.
  float capacity_Kc_s = 0.1f;
  // Settling velocity of the suspended grade, m/s -- the DEPOSITION rate's
  // own clock, and NOT a free knob: it is the same quantity
  // `adaptation_length_m`'s comment already derives that length from
  // (L = u*h/w_s, "physical, silt-grade suspended load: u*h/w_s =
  // 1*0.3/1e-3 -> ~300 m"). 1e-3 m/s is that comment's own w_s, so the two
  // knobs stay consistent by construction rather than by coincidence.
  //
  // WHY DEPOSITION NEEDS ITS OWN RATE AT ALL (the brief allows a k_deposit
  // "if the formulation needs it beyond the adaptation-length factor"): the
  // adaptation-length rate is min(1, |v|*dt/L) and therefore vanishes
  // identically as |v| -> 0. Using it for deposition would forbid the one
  // behaviour the design explicitly requires -- suspended load settling out
  // over STILL water (a lake floor) and over DRY ground (where phase-0's
  // leftover `sus` has to land). The same relaxation physics gives the
  // deposition rate directly and WITHOUT the velocity: substituting
  // L = u*h/w_s into |v|*dt/L at |v| = u collapses it to w_s*dt/h. So this
  // is not a second free parameter, it is the same one written in the
  // variables deposition actually depends on.
  float sus_settling_velocity_m_per_s = 1e-3f;
  // Transverse-slope deflection of the sediment flux (Ikeda / Struiksma's
  // closure), DIMENSIONLESS: the sediment transport direction deviates from
  // the flow direction toward the downhill side of the TRANSVERSE bed slope
  // by tan(deviation) = transverse_slope_coeff * (transverse bed slope).
  // This is the mechanism that seeds meandering -- it is what moves material
  // toward the inside of a bend and builds the point bar that steers the
  // flow further. 1.0 is mid-range for the classic closure's 1/f(theta),
  // which sits at ~0.5-2 for ordinary Shields numbers; at a 5% transverse
  // slope it deflects the flux by ~2.9 degrees. The closure ROTATES the
  // transport vector and does not lengthen it -- SedAdvect renormalises back
  // to the undeflected speed, see there.
  float transverse_slope_coeff = 1.0f;
  // Talus (avalanche) relaxation rate, PER YEAR: the e-folding rate at which
  // bed slope in excess of `repose_angle_deg` is shed downhill. The talus
  // pair sheds min(1, rate*dt_bed) of its stability-capped amount per pass,
  // so this is what puts that pass on a CLOCK at all.
  //
  // WHY IT NEEDED ONE, AND WHY IT IS THE **BED** CLOCK. Before this the pass
  // shed a fixed fraction of the excess PER CALL, with no dt and no morfac in
  // it -- so the hillslope/fluvial ratio silently tracked `swe_substeps`,
  // `morfac` and the grid resolution, none of which are physics. Talus is a
  // BED process (it moves substrate, not water), and under MORFAC the bed
  // advances `morfac` times the fluid clock, so talus must advance with it:
  // otherwise a run at morfac 300 models 300 cycles of river incision against
  // one cycle of hillslope response, and produces gorges with no talus aprons
  // purely as an artefact of the accelerator. THIS IS THE SECOND AND LAST
  // PLACE `morfac` ENTERS (the first is ClampMorfacBedDelta); both are the
  // same operation -- converting a fluid-clock quantity onto the bed clock --
  // and neither is on the `sus` side.
  //
  // NOT `Params::settling`, which the first version of this pass borrowed
  // without saying so. That knob is phase-0 `Cascade`'s per-visit settle
  // fraction, a dimensionless share of a serial in-place relaxation with no
  // clock behind it; reusing it silently coupled two passes that have
  // different discretisations and different meanings. Phase 1 gets its own
  // rate, in real units, and `settling` is left to phase 0.
  //
  // 1.0/yr: an over-steepened regolith slope relaxes toward repose on the
  // order of a year -- fast against every landform timescale here, slow
  // against a fluid cycle. That is the right shape, because repose is a
  // CONSTRAINT the landscape is pushed back toward rather than a slow process
  // that shapes it. A DELIBERATE consequence: at morfac 1 a cycle covers
  // minutes of real time, so the pass is very nearly inert -- correct physics
  // (nothing avalanches in five minutes), and the reason the invariance
  // fixtures that mean to COVER talus raise this knob explicitly rather than
  // hoping the default bites.
  float talus_relaxation_per_yr = 1.0f;

  // THE landscape clock. One step represents this many years, and every
  // process scales with it: diffusion through k = D*dt/cell^2, and erosion
  // through the water a step delivers (see EffectiveDropVolume). Previously
  // erosion was step-based and diffusion was year-based, so a run could not
  // state how long it represented and D had no calibration to hang on.
  float dt_years = 200.0f;

  std::string out = "proto_out";
  // Phase-0 STEP cadence. REINTERPRETED as a phase-1 CYCLE cadence (Task 7)
  // once `cycles > 0`: the driver runs RunSweCycles in batches of this many
  // cycles, dumping a phase-1 snapshot after each batch (and always after
  // the last one, whatever `cycles % snapshot_every` is) -- the same
  // "always land on the final one" guarantee RunSim's own step loop already
  // gives phase 0. One field, two clocks, because both are "how often do I
  // want to see this run" and a run only ever occupies one phase at a time.
  int snapshot_every = 250;
  // Phase-1 cycle count (Task 7). 0 (the default) leaves phase 1 off
  // entirely -- the driver only calls SweWarmStart/RunSweCycles when this is
  // nonzero, which is what keeps a `--cycles 0` run's output byte-shaped
  // exactly like it was before phase 1 existed (see main()'s own comment).
  // Read by the driver alone; RunSweCycles takes its cycle count as an
  // explicit argument rather than from here, so this field exists purely to
  // fit the CLI's uniform "--flag sets a p.field" pattern.
  int cycles = 0;
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
  // every existing read of it is untouched. `soil_b` is the Jacobi back
  // buffer the phase-1 morpho passes need: SedExchange and TalusApply write
  // bed state while OTHER cells are still reading it (the energy-slope
  // stencil, the neighbour talus gather), so they cannot update in place the
  // way phase-0's serial `Deposit`/`Erode` do -- height/soil/sus all
  // ping-pong together and swap at the end of each pass.
  std::vector<float> soil, soil_b;
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

  // --- phase-1 morphodynamic state (protogen_swe.cpp, Task 6) ------------
  //
  // Talus (thermal-erosion) transfer buffer: per cell, the volume-per-area
  // (METRES of solid, SI like every other quantity in protogen_swe.cpp) it
  // wants to shed to each of its EIGHT neighbours this pass. Written by
  // TalusFlux, gathered by TalusApply -- the Jacobi twin of phase-0's serial
  // in-place `Cascade`. Neighbour order is TalusApply's own kTdx/kTdy
  // (row-major from (-1,-1) to (+1,+1)), whose opposite-index is simply
  // 7 - k.
  std::vector<std::array<float, 8>> talus;
  // SEDIMENT ledger, cubic metres of solid. Three terms, and they are not
  // interchangeable:
  //   `swe_sed_border_export_m3`  solid advected off the open border by
  //                               SedAdvect, computed from the border faces'
  //                               own water flux times the cell's sediment
  //                               concentration -- an INDEPENDENT physical
  //                               estimate, deliberately not "whatever went
  //                               missing", so the mass tripwire has
  //                               something real to check the field against.
  //   `swe_sed_morfac_created_m3` solid the MORFAC staggering created (+, net
  //                               deposition) or destroyed (-, net erosion):
  //                               the bed moves M times as far as the `sus`
  //                               it exchanged with, by design (see
  //                               Params::morfac), so Sigma(bed+sus) is NOT
  //                               conserved at M != 1 and this is exactly the
  //                               term that says by how much. Identically
  //                               zero at M = 1.
  //   `swe_sed_advect_fix_max`    diagnostic, not a ledger term: the largest
  //                               |renormalisation factor - 1| SedAdvect's
  //                               mass fixer had to apply in any one cycle.
  //                               Semi-Lagrangian advection is not
  //                               conservative on its own; this says how hard
  //                               the fixer had to work, and a value far from
  //                               0 means the advection step is too long for
  //                               the flow field.
  //   `swe_sed_advect_fix_residual_m3`
  //                               solid the mass fixer could NOT place,
  //                               because its gain hit `kMaxAdvectFixFactor`
  //                               or because the gather collapsed to an empty
  //                               field. Signed. It should be exactly 0 on
  //                               any healthy run; a nonzero value is a
  //                               reported failure of the advection step
  //                               rather than a quietly balanced book, and
  //                               the audit subtracts it as a known term so
  //                               it cannot masquerade as conservation.
  double swe_sed_border_export_m3 = 0.0;
  double swe_sed_morfac_created_m3 = 0.0;
  double swe_sed_advect_fix_residual_m3 = 0.0;
  double swe_sed_advect_fix_max = 0.0;

  explicit Grid(int res)
      : n(res), cells(size_t(res) * res), height(cells, 0.f),
        height_b(cells, 0.f),
        vol_ema(cells, 0.f), vol_ema_b(cells, 0.f),
        discharge(cells, 0.f), discharge_b(cells, 0.f),
        Qm3s(cells, 0.f), Qm3s_b(cells, 0.f),
        momx(cells, 0.f), momy(cells, 0.f), momx_b(cells, 0.f), momy_b(cells, 0.f),
        soil(cells, 0.f), soil_b(cells, 0.f),
        sus(cells, 0.f), sus_b(cells, 0.f),
        vol_track(cells, 0.f), mx_track(cells, 0.f), my_track(cells, 0.f),
        visits(cells, 0u),
        h(cells, 0.f), h_b(cells, 0.f),
        flux(cells, std::array<float, 4>{0.f, 0.f, 0.f, 0.f}),
        velx(cells, 0.f), vely(cells, 0.f),
        talus(cells, std::array<float, 8>{0.f, 0.f, 0.f, 0.f,
                                          0.f, 0.f, 0.f, 0.f}) {}

  inline size_t idx(int x, int y) const { return size_t(y) * n + x; }
  inline bool oob(int x, int y) const {
    return x < 1 || y < 1 || x >= n - 1 || y >= n - 1;
  }
};

// Extracted from main so the sanity tests exercise the REAL loop rather than
// a copy that can drift from it.
struct SimStats {
  double t_drops = 0, t_grid = 0, t_settle = 0;
  // One-shot phase-0 -> phase-1 handoff (Task 5's SweWarmStart). NOT filled
  // in by RunSweCycles -- SweWarmStart takes no SimStats*, it runs exactly
  // once per run, so the driver (Task 7's main()) times it directly around
  // the one call site instead.
  double t_swe_warm_start = 0;
  // Phase-1 SWE cycle timers, one bucket per dispatch-list pass plus the
  // per-cycle CFL/tripwire reduction. RunSweCycles fills them in when given
  // a non-null SimStats*; wired into the driver as of Task 7.
  double t_swe_reduce = 0, t_swe_flux = 0, t_swe_depth = 0, t_swe_velocity = 0;
  // Morpho hook buckets (Task 6), same shape and same caveat: filled in by
  // RunSweCycles when given a non-null SimStats*, zero while `morfac` is 0
  // (the hook does not run at all then -- see RunSweCycles). Talus is TWO
  // passes (TalusFlux, the per-cell demand; TalusApply, the gather) and, as
  // of Task 7's perf instrumentation, gets two buckets rather than the one
  // combined `t_swe_talus` Task 6 shipped with -- the split costs one more
  // chrono call per cycle (cheap; this hook runs once per cycle, not once
  // per substep, so it has none of the per-substep overhead concern
  // RunSweCycles' own comment raises about the fluid passes) and is what
  // lets the perf table name all FOUR morpho passes instead of three.
  double t_swe_sed_exchange = 0, t_swe_sed_advect = 0;
  double t_swe_talus_flux = 0, t_swe_talus_apply = 0;
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

// Phase-1 morphodynamic passes (protogen_swe.cpp, Task 6). Same contract as
// the fluid passes above -- standalone gather-only Jacobi over ping-pong
// buffers, no atomics, extending the frozen GPU dispatch list. RunSweCycles
// runs them ONCE per cycle, after that cycle's `swe_substeps` fluid substeps,
// in exactly this order:
//
//   SedExchange -> SedAdvect -> TalusFlux -> TalusApply
//
// NO PASS CALLS ANOTHER; the sequencing lives entirely in RunSweCycles.
// `dt_morpho` is the cycle's TOTAL fluid time (swe_substeps * dt) -- these
// passes stand in for the sediment work of the substeps they representatively
// skipped, so they run on the fluid clock, not on a clock of their own. The
// only quantity on the accelerated BED clock is the bed-elevation delta
// inside SedExchange, and it gets there through `ClampMorfacBedDelta` alone.
// `dt_bed_s` is the BED-clock interval, `morfac * dt_morpho` -- see
// Params::talus_relaxation_per_yr for why the talus pair runs on the bed
// clock and not the fluid one.
void SedExchange(Grid& g, const Params& p, float dt_morpho);
void SedAdvect(Grid& g, const Params& p, float dt_morpho);
void TalusFlux(Grid& g, const Params& p, float dt_bed_s);
void TalusApply(Grid& g, const Params& p);

// The transport capacity SedExchange itself uses -- THE single definition,
// exported (rather than duplicated in the test file) so FlatReachTransport,
// StillLakeInert and ExnerCapacityLimiter assert against the same arithmetic
// the pass runs, not a hand-copy that could drift. Returns an EQUIVALENT
// SEDIMENT DEPTH in metres (see Params::capacity_Kc_s). Reads `height`, `h`
// (own cell and its four orthogonal neighbours, for the central-difference
// energy slope) and `velx`/`vely` (own cell only); writes nothing.
float SedCapacityM(const Grid& g, const Params& p, int x, int y);

// The two-layer substrate's erosion yield, in METRES of solid, for a demand
// of `demand_m` against `soil_m` metres of cover -- soil first at full rate,
// then the RESIDUAL demand scaled by `bedrock_erodibility` before it bites
// bedrock. The phase-1 counterpart of protogen.cpp's `Erode` (same law, no
// side effects), exported so ExnerSoilBedrockConservation can pin the exact
// split arithmetic independently of the fixture that drives it.
float SedSubstrateYieldM(float demand_m, float soil_m, const Params& p);

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
// state, a CFL-derived dt collapsed below `dt_floor_s`, or (Task 6) the
// periodic sediment-mass audit drifting past its tolerance. `aborted_cycle`/
// `reason` name where and why, so a caller (test or, from Task 7, the
// driver) can report it rather than merely detect it. `aborted_cycle` is
// always in the caller's GLOBAL cycle numbering -- see `cycle_offset` on
// RunSweCycles below -- not merely relative to whichever call found it.
struct SweRunResult {
  bool ok = true;
  int aborted_cycle = -1;
  std::string reason;
};

// The sediment-mass audit's starting point (Task 7 fix round 1). Captured
// ONCE -- either internally, self-contained, when a RunSweCycles caller
// passes no `audit` (every existing call site, including every test in
// protogen_tests.cpp), or externally when a caller threads the SAME
// `SweAuditBaseline` object through SEVERAL RunSweCycles calls on the same
// Grid.
//
// THE SECOND SHAPE IS WHAT MAIN()'S `--snapshot-every` BATCHING NEEDS, AND
// GETTING IT WRONG IS A REAL BUG THIS STRUCT FIXES. RunSweCycles's audit
// (see its own comment) checks "does the ledger explain the CHANGE in solid
// volume since the baseline". If every batch call captured its OWN baseline
// (the pre-fix shape), a leak that stays under `kMassAuditRelTol` WITHIN one
// `--snapshot-every` window is invisible BY CONSTRUCTION: the next batch's
// fresh baseline captures the state AFTER the leak, so the leak becomes part
// of "how things always were" for that window and is never compared against
// the true start of phase 1. A small, steady leak that would trip a single
// un-batched call over the same total cycle count could then survive an
// entire batched run undetected, purely because of how the run happened to
// be chunked for snapshotting -- which defeats the audit's whole purpose.
// BatchedMassAuditCatchesLeak (protogen_tests.cpp) demonstrates the
// difference directly: the identical leak, at the identical point, trips
// when threaded through one `SweAuditBaseline` across two batches and does
// not when each batch gets its own (the pre-fix shape).
struct SweAuditBaseline {
  bool captured = false;
  double solid_m3 = 0.0;
  double base_export_m3 = 0.0;
  double base_created_m3 = 0.0;
  double base_fix_residual_m3 = 0.0;
};

// Runs `cycles` SWE cycles (CFL dt -> swe_substeps x Flux/Depth/Velocity ->
// morpho hook) starting from Grid `g`'s current `h`/`flux` state --
// callable standalone, independent of RunSim/phase-0 (Task 5 supplies the
// warm start that seeds `h` from phase-0's finished bed; nothing here
// requires it). `stats`, if non-null, accumulates per-pass wall time.
//
// THE MORPHO HOOK IS GATED ON `p.morfac > 0`: a run with morfac == 0 is a
// FLUID-ONLY run, and every pre-Task-6 SWE fixture in protogen_tests.cpp
// (which are all statements about the fluid alone) sets it explicitly for
// that reason. It is not an enable_* toggle in disguise -- morfac == 0
// literally means "no bed change per fluid cycle", so skipping the passes
// and running them to produce exactly zero are the same landscape, only one
// of them costs nothing.
//
// `audit` and `cycle_offset` (Task 7 fix round 1) exist for a caller that
// runs this in SEVERAL calls on the same Grid -- main()'s `--snapshot-every`
// batching, and nothing else today. Both default to "this call is the whole
// story": `audit = nullptr` self-contained-baselines exactly as before (see
// SweAuditBaseline's own comment), and `cycle_offset = 0` numbers this
// call's own cycles from 0. A caller chunking `N` total cycles into batches
// must pass the SAME `SweAuditBaseline*` to every call and set
// `cycle_offset` to how many cycles already ran in EARLIER batches, so
// `aborted_cycle`/the abort snapshot/the audit's tolerance window are all in
// the same global numbering regardless of how the run was chunked.
SweRunResult RunSweCycles(Grid& g, const Params& p, int cycles,
                          SimStats* stats = nullptr,
                          SweAuditBaseline* audit = nullptr,
                          int cycle_offset = 0);

// ONE OF THE TWO PLACES `p.morfac` IS EVER APPLIED (the other is the talus
// pair's bed-clock interval in RunSweCycles -- see
// Params::talus_relaxation_per_yr). Both are the SAME operation, converting a
// fluid-clock quantity onto the accelerated bed clock; neither is ever on the
// `sus` side, which is the whole point of the staggered scheme.
//
// Scales a proposed bed-elevation
// delta (metres, either sign, on the FLUID clock) by `p.morfac`, then bounds
// the magnitude to `kMaxBedDeltaFraction * depth_m` so one fluid cycle's
// accelerated bed change can never exceed a fixed fraction of the water
// sitting on top of it (see kMaxBedDeltaFraction for why that bound is what
// makes MORFAC legitimate). ACTIVE as of Task 6: `SedExchange` is the sole
// caller, on both the erosion and the deposition side. In particular the
// `sus` side of the exchange never scales by `morfac` (`sus` lives on the
// fluid clock, the bed on the accelerated one) -- that asymmetry IS the
// staggered scheme.
// Exported (not a private helper in protogen_swe.cpp) so a test can exercise
// its clamp behaviour directly.
float ClampMorfacBedDelta(float raw_delta_m, float depth_m, const Params& p);

// -------------------------------------------- output boundary (Task 7) -----
//
// THE ONLY PLACE IN PROTOGEN A LAKE/RIVER DISTINCTION EXISTS. Phase 1's SWE
// state makes none: `h` is a depth wherever the cell is wet, channel or
// lake alike, exactly like the real shallow-water equations it approximates.
// Everything downstream that wants "standing water" specifically -- the
// `water.f32` raster, world.txt's `surface`/`lake_tag`, SoilCutoffs' dry-cell
// filter -- needs a RULE to build that from, not a field the sim already
// carries. This is that rule; see ClassifyBoundaryWater's own comment
// (protogen.cpp) for the seed/grow algorithm.
//
// SEED thresholds. A cell only STARTS a lake region if it is both:
//   DEEP    -- h > kLakeSeedDepthM. 1 m clears every ordinary channel depth
//              this sim produces (SweWarmStart's Manning seeding and the
//              fluid solver's own steady states both run in the decimetre
//              range at production discharges -- see SweManningConvergence's
//              own measured h) while sitting far under a real lake's depth
//              (this codebase's own README records lakes over 300 m deep at
//              16 km scale), so it does not accidentally seed a merely-deep
//              flood pulse in an otherwise ordinary channel.
//   SLOW    -- speed < kLakeSeedSpeedMPerS. A lake carries no through-flow
//              (StillLakeInert proves the fluid solver holds a genuinely
//              still lake to bit-exact stillness), while a channel needs
//              real velocity to be a channel at all -- Manning speeds at
//              ordinary production slopes/roughness run from a few cm/s to
//              several m/s. 0.05 m/s sits comfortably below that whole range
//              while staying well above any residual numerical creep a
//              near-equilibrium lake might carry.
// Both conditions matter independently: a deep-but-fast cell (a flood pulse
// in a channel) and a shallow-but-slow one (a puddle) must each fail to
// seed on their own -- BoundaryClassification (protogen_tests.cpp) pins
// both.
constexpr float kLakeSeedDepthM = 1.0f;
constexpr float kLakeSeedSpeedMPerS = 0.05f;
// GROWTH threshold, once a seed exists: a 4-connected flood fill over the
// wet component, crossing into a neighbour only when its WATER SURFACE
// (bed + h) is within this of the cell it came from. A lake's surface is
// flat by construction, so two adjacent lake cells always pass however
// different their bed elevation (a shallow margin next to the deep centre
// included -- this is what makes the margins join the region without
// needing a depth test of their own). A channel cannot: SweFlux's own
// acceleration term (dt*g*A_over_L*dhead) needs a real head DIFFERENCE
// between adjacent cells to drive flow at all, and at production cell
// sizes that runs in the DECIMETRE range per cell -- an order of magnitude
// or more above this CENTIMETRE bound -- so growth stops at the true
// shoreline without ever asking "is this a channel", only "is the surface
// still flat".
constexpr float kLakeSurfaceContinuityM = 0.01f;

// Returns a depth raster, `g.cells` long: `g.h[i]` where cell `i` was
// classified as lake, 0 elsewhere. Deterministic (the underlying seed/grow
// reachability is a pure function of the grid state, independent of
// traversal order -- see the .cpp for the argument). For a phase-0-only
// grid (h identically 0, since phase-0 never touches it) this returns all
// zero, which is what keeps a `--cycles 0` run's `water.f32` byte-identical
// to the old hardcoded-zero placeholder.
std::vector<float> ClassifyBoundaryWater(const Grid& g, const Params& p);

}  // namespace pg

// The test entry point. Defined in protogen_tests.cpp; called from main().
namespace test {
int RunAll();
}
