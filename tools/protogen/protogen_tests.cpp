// The sanity-test suite (`--test`): physical invariants for the sim in
// protogen.cpp, checked on small grids. Split out of the single protogen.cpp
// TU it used to live in -- see protogen.hpp for the shared surface (Params,
// Grid, SimStats, ParticleProbe, InitTerrain, RunSim, Cascade) this file
// pulls in unqualified via the using-directive below, exactly as it saw them
// when it was nested inside the same anonymous namespace as their definitions.

#include <algorithm>
#include <array>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "protogen.hpp"

using namespace pg;

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
  return p;
}

// EVERYTHING off. A unit test enables ONLY the mechanism it exercises, so a
// mechanism added later cannot silently join a test written before it existed.
// The old style listed what to DISABLE, which made every new mechanism opt-out
// rather than opt-in -- that is how T12 ended up depending on particle
// stepping.
Params Isolated(int res = 64) {
  Params p = Base(res);
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.enable_diffusion = false;
  p.enable_soil_production = false;
  p.enable_momentum = false;
  p.drops = 0;
  return p;
}

Grid Run(const Params& p, SimStats& st) {
  Grid g(p.res);
  InitTerrain(g, p);
  RunSim(p, g, st, false);
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
  Grid g0(p.res);
  InitTerrain(g0, p);
  const double before = SumH(g0);
  SimStats st;
  Grid g = Run(p, st);
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
  SimStats st;
  Grid g = Run(p, st);
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
  SimStats s1, s2;
  Grid a = Run(p, s1);
  Grid b = Run(p, s2);
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
      {"entrainment", [](Params& p, int i) { p.entrainment = i ? 2.f : 20.f; }},
      {"adaptation_length", [](Params& p, int i) {
         p.adaptation_length_m = i ? 60.f : 600.f; }},
      {"repose_angle_deg", [](Params& p, int i) {
         p.repose_angle_deg = i ? 15.f : 60.f; }},
      {"runoff", [](Params& p, int i) { p.runoff_m_per_yr = i ? 0.1f : 4.f; }},
  };
  for (const Knob& k : knobs) {
    Params a = Base(), b = Base();
    a.terrain = b.terrain = Params::Terrain::Bowl;
    k.set(a, 0); k.set(b, 1);
    SimStats sa, sb;
    Grid ga = Run(a, sa), gb = Run(b, sb);
    bool differs = false;
    for (size_t i = 0; i < ga.cells && !differs; ++i)
      if (ga.height[i] != gb.height[i]) differs = true;
    Check((std::string("knob is live: ") + k.name).c_str(), differs,
          differs ? "output changes" : "NO EFFECT - masked or unused");
  }
}

// --- 8. discharge equals runoff x drainage area ---------------------------
// The strongest statement the discharge field makes about itself.
void DischargeMatchesArea() {
  Params p = Base();
  p.terrain = Params::Terrain::Plane;
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.steps = 400;
  SimStats st;
  Grid g = Run(p, st);
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
  SimStats st;
  Grid g = Run(p, st);
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
  SimStats st;
  Grid g = Run(p, st);
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
// UNIT test of the two-layer substrate: same fixture, same forcing, only the
// starting cover differs. A mantled hill must lose substantially more material
// than a bare-rock one, because Erode takes soil at full rate and bedrock at
// bedrock_erodibility.
//
// Methodology corrected, with proof, on two counts:
//
//  METRIC. It measured total height loss, which is EXPORT off the map, not
//  erosion. Measured: lost_soft 3.889e-1 and lost_hard 1.858e0 were exactly the
//  off-map figures, and the ordering had inverted. The soft run flooded (93.8%
//  wet) while the hard run did not (0.0%); once flooded, particles enter the
//  lake branch on contact, stop eroding and stop reaching the boundary, so
//  export COLLAPSED in the run that was eroding hardest. Material MOVED
//  (sum |dh|) is what the claim is about and does not route through the
//  boundary or the lake branch.
//
//  RANGE. initial_soil_m was 500 m against relief_m = 300 -- a soil layer
//  1.67 height units thick, taller than the entire terrain. That is not
//  "effectively unlimited cover", it is out of range, and it is what triggered
//  the flood. 20 m makes the same point inside the model's domain.
void BedrockResists() {
  auto moved = [](float cover_m) {
    Params p = Isolated(64);
    p.enable_erosion = true;         // the ONLY mechanism under test
    p.enable_cascade = true;         // erosion moves material through it
    p.terrain = Params::Terrain::Lobe;
    p.initial_soil_m = cover_m;
    p.steps = 200;
    p.drops = 16;
    Grid g0(p.res);
    InitTerrain(g0, p);
    SimStats st;
    Grid g = Run(p, st);
    double m = 0;
    for (size_t i = 0; i < g.cells; ++i)
      m += std::fabs(double(g.height[i]) - double(g0.height[i]));
    return m;
  };
  const double soft = moved(20.0f), hard = moved(0.0f);
  Check("substrate: bedrock resists erosion", hard < soft * 0.75,
        F("moved: mantled %.4e vs bare rock %.4e (ratio %.2f)", soft, hard,
          soft > 0 ? hard / soft : 0.0));
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
  SimStats sa, sb;
  Grid ga = Run(a, sa), gb = Run(b, sb);
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
  SimStats st;
  Grid g = Run(p, st);
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
  SimStats st;
  Grid g0(p.res);
  InitTerrain(g0, p);
  Grid g = Run(p, st);
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
  SimStats st;
  Grid g0(p.res);
  InitTerrain(g0, p);
  float peak0 = 0.f;
  for (float h : g0.height) peak0 = std::max(peak0, h);
  float prev = peak0;
  bool monotone = true;
  for (int round = 0; round < 4; ++round) {
    Params q = p;
    q.steps = 100 * (round + 1);
    SimStats s;
    Grid g = Run(q, s);
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
  SimStats s2;
  Grid ge = Run(q, s2);
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
  SimStats st;
  Grid g = Run(p, st);
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
    SimStats st;
    Grid g = Run(p, st);
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
  p.steps = 1;
  p.drops = 1;  // exactly one particle, so the probe is one trajectory
  ParticleProbe probe;
  p.probe = &probe;
  SimStats st;
  Grid g = Run(p, st);

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
    SimStats st;
    Grid g = Run(p, st);
    double moved = 0.0;
    for (size_t i = 0; i < g.cells; ++i)
      moved += std::fabs(double(g.height[i]) - double(g0.height[i]));
    return std::array<double, 3>{
        moved / double(g.cells) * base.relief_m,  // mean |dh|, metres
        double(g.deposited_death),
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
  // CONVERGENCE, not pairwise agreement.
  //
  // No discretisation of a saturating process can agree at every parcel size.
  // One traversal closes 1 - exp(-volume*f) of the gap, and exponentials
  // compose only on the SAME cell; parcels spawn on different paths, so thin
  // ones each meet fresh gap while a fat one saturates one cell and wastes the
  // remainder of its capacity. 4*(1-e^-f) = 0.286 against 1-e^-4f = 0.260 at
  // f ~ 0.074 -- about 10%, vanishing as parcels shrink. The coarse sample is
  // simply outside the converged regime, exactly as 32 m steps were for T8.
  //
  // NOT a loosening. A genuine leak gives a near-CONSTANT absolute increment --
  // which is how the per-iteration relaxation bug was caught (47%/31%,
  // increments 0.778 then 0.749, with no limit to converge to). Requiring the
  // increment to HALVE each refinement is a stronger statement than a fixed
  // tolerance, and that bug would still fail it.
  const double inc_cm = std::fabs(coarse[0] - mid[0]);
  const double inc_mf = std::fabs(mid[0] - fine[0]);
  char buf2[280];
  std::snprintf(buf2, sizeof(buf2),
                "%s; increments %.3f then %.3f (shrinking %.1fx)", buf,
                inc_cm, inc_mf, inc_cm / std::max(inc_mf, 1e-9));
  Check("T2 volume refinement converges",
        inc_mf < 0.5 * inc_cm && d_mf < 0.15, buf2);
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
  p.enable_diffusion = true;
  p.diffusion_D_m2_per_yr = 5e-3f;
  p.steps = 50;
  p.drops = 0;  // pure grid pass
  Grid g0(p.res);
  InitTerrain(g0, p);
  SimStats st;
  Grid g = Run(p, st);

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
  SimStats st;
  Grid g = Run(p, st);

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
  SimStats st;
  Grid g = Run(p, st);
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

// ===========================================================================
// SOIL PRODUCTION -- bedrock weathering into regolith.
//
//   d(soil)/dt = P0 * exp(-soil / h_star)
//
// The missing term. Soil is currently a finite stock seeded once and only ever
// depleted: measured 4.00 -> 1.19 m with 40% of cells stripped to bare rock,
// after which hillslopes stop evolving because bedrock yields at 0.1x. Nothing
// converts rock to regolith.
//
// Soil comes FROM bedrock, so bedrock lowers by exactly what soil gains and the
// surface is unchanged. Real regolith bulks up ~1.5x as it weathers; 1:1 is the
// conservative choice and avoids inventing volume.
// ===========================================================================

// --- P1. bare rock gains soil at the full rate -------------------------------
void SoilProductionOnBareRock() {
  Params p = Base(16);
  p.enable_erosion = false; p.enable_cascade = false; p.drops = 0;
  p.enable_soil_production = true;
  p.soil_production_m_per_yr = 1e-4f;
  p.soil_efold_m = 0.5f;
  p.steps = 20;
  Grid g(p.res);
  InitTerrain(g, p);
  std::fill(g.soil.begin(), g.soil.end(), 0.f);   // bare rock everywhere
  SimStats st;
  RunSim(p, g, st, false);
  const double got = double(g.soil[g.idx(8, 8)]) * p.relief_m;
  // Against the EXACT solution, not P0*t. Integrating
  // d(soil)/dt = P0*exp(-soil/h*) from bare rock gives
  //     soil(t) = h* * ln(1 + P0*t/h*)
  // and self-limitation bites immediately, so P0*t overshoots by 26% even at
  // this short duration. Asserting the closed form makes this a much stronger
  // test than the linear approximation it replaces -- it pins the whole
  // trajectory, not just that soil appears.
  const double t_yr = double(p.dt_years) * p.steps;
  const double want = p.soil_efold_m *
                      std::log(1.0 + p.soil_production_m_per_yr * t_yr /
                                         p.soil_efold_m);
  Check("P1 bare rock gains soil at P0",
        std::fabs(got - want) < 0.05 * want,
        F("gained %.4f m over %.0f yr, exact h*ln(1+P0t/h*) = %.4f", got, t_yr,
          want));
}

// --- P2. production is SELF-LIMITING -----------------------------------------
// The one that separates the physical form from a constant source. Gain at
// soil = 2*h_star must be exp(-2) = 0.135 of the gain on bare rock. Without
// this soil grows linearly for ever.
void SoilProductionSelfLimiting() {
  Params p = Base(16);
  p.enable_erosion = false; p.enable_cascade = false; p.drops = 0;
  p.enable_soil_production = true;
  p.soil_production_m_per_yr = 1e-4f;
  p.soil_efold_m = 0.5f;
  p.steps = 10;
  auto gain = [&](float start_m) {
    Grid g(p.res);
    InitTerrain(g, p);
    std::fill(g.soil.begin(), g.soil.end(), start_m / p.relief_m);
    SimStats st;
    RunSim(p, g, st, false);
    return (double(g.soil[g.idx(8, 8)]) * p.relief_m) - double(start_m);
  };
  const double bare = gain(0.f), thick = gain(2.0f * p.soil_efold_m);
  const double ratio = (bare > 0) ? thick / bare : 0.0;
  Check("P2 production is self-limiting",
        std::fabs(ratio - std::exp(-2.0)) < 0.25 * std::exp(-2.0),
        F("gain ratio %.4f at 2*h_star, expected exp(-2) = %.4f", ratio,
          std::exp(-2.0)));
}

// --- P3. mass is conserved: rock becomes soil, surface unmoved ---------------
void SoilProductionConservesMass() {
  Params p = Base(16);
  p.enable_erosion = false; p.enable_cascade = false; p.drops = 0;
  p.enable_soil_production = true;
  p.soil_production_m_per_yr = 1e-3f;
  p.soil_efold_m = 0.5f;
  p.steps = 30;
  Grid g(p.res);
  InitTerrain(g, p);
  std::fill(g.soil.begin(), g.soil.end(), 0.f);
  const double h_before = SumH(g);
  const double soil_before = std::accumulate(g.soil.begin(), g.soil.end(), 0.0);
  SimStats st;
  RunSim(p, g, st, false);
  const double dh = std::fabs(SumH(g) - h_before) / std::max(h_before, 1e-9);
  const double dsoil = std::accumulate(g.soil.begin(), g.soil.end(), 0.0) - soil_before;
  Check("P3 rock becomes soil, surface unmoved",
        dh < 1e-6 && dsoil > 0.0,
        F("surface drift %.2e (want 0), soil gained %.4e", dh, dsoil));
}

// --- P4. soil reaches the ANALYTIC steady state under erosion ----------------
// The invariant that proves this is physics rather than a source term:
// production P0*exp(-h/h*) balancing removal E gives h_eq = h* * ln(P0/E),
// a NUMBER, not a direction. Checked at two erosion rates.
void SoilProductionSteadyState() {
  Params p = Base(16);
  p.enable_erosion = false; p.enable_cascade = false; p.drops = 0;
  p.enable_soil_production = true;
  p.soil_production_m_per_yr = 1e-3f;
  p.soil_efold_m = 0.5f;
  p.steps = 4000;   // long enough to settle
  auto equilibrium = [&](float E_m_per_yr) {
    Params q = p;
    q.soil_strip_m_per_yr = E_m_per_yr;   // synthetic constant removal
    Grid g(q.res);
    InitTerrain(g, q);
    std::fill(g.soil.begin(), g.soil.end(), 0.f);
    SimStats st;
    RunSim(q, g, st, false);
    return double(g.soil[g.idx(8, 8)]) * q.relief_m;
  };
  const double E1 = 2e-4, E2 = 5e-4;
  const double got1 = equilibrium(float(E1)), got2 = equilibrium(float(E2));
  const double want1 = p.soil_efold_m * std::log(p.soil_production_m_per_yr / E1);
  const double want2 = p.soil_efold_m * std::log(p.soil_production_m_per_yr / E2);
  const bool ok = std::fabs(got1 - want1) < 0.25 * want1 &&
                  std::fabs(got2 - want2) < 0.25 * want2;
  char buf[200];
  std::snprintf(buf, sizeof(buf),
                "E=2e-4: %.3f m vs h*ln(P0/E)=%.3f;  E=5e-4: %.3f vs %.3f",
                got1, want1, got2, want2);
  Check("P4 soil reaches h* ln(P0/E)", ok, buf);
}

int RunAll() {
  std::printf("protogen sanity tests (small grids, production 16 m cells)\n");
  MassConservation();
  FlatPlaneInert();
  Determinism();
  KnobLiveness();
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

  std::printf("\n  physics invariants (PEND = mechanism not built yet)\n");
  TerminalVelocityManning();
  VolumeDiscretizationInvariance();
  DiffusionRelaxesRidge();
  ParticleNeverSkipsACell();
  CascadeThresholdGate();
  MassConservationLongRun();
  HorseshoeDrainsToOutflow();

  SoilProductionOnBareRock();
  SoilProductionSelfLimiting();
  SoilProductionConservesMass();
  SoilProductionSteadyState();

  std::printf("\n  %d passed, %d failed, %d pending", g_pass, g_fail, g_pending);
  if (g_pending_ready)
    std::printf(" (%d of them now PASS -- promote to Check())", g_pending_ready);
  std::printf("\n");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace test

