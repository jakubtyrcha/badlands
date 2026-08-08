// THROWAWAY PROTOTYPE -- particle hydraulic erosion (SimpleHydrology / Nick
// McDonald, nickmcd.me 2023-12-12).
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
// tuned constants keep their meaning. Physical units are confined to
// REPORTING (metres on dump).
//
// What is new relative to the reference:
//   * parallel over particles via the repo's Taskflow pool, deterministically
//   * phase-1 shallow-water + Exner morphodynamics (protogen_swe.cpp), the
//     phase-0 -> phase-1 handoff below, and the output boundary this file
//     pulls in from src/mapgen/ -- see tools/protogen/README.md.
//
// NOT a single-TU build any more (Task 2 split it): this file is phase-0 +
// the whole-sim driver + the output boundary; protogen_swe.cpp is the
// phase-1 SWE/morpho passes; protogen_tests.cpp is the `--test` suite. No
// CMake target either way -- Taskflow is header-only, so hand-listing the
// TUs on one `c++` invocation is still simpler than a build system entry.
// The exact, current build line is tools/protogen/README.md's "Build and
// run" section, not repeated here where it would drift out of sync.

#include <algorithm>
#include <array>
#include <numeric>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "protogen.hpp"

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

using namespace pg;

namespace {

// Real gravity. The motion law integrates a genuine velocity in m/s now, so
// this is an acceleration, not the reference's dimensionless force scale.
constexpr float kGravityMS2 = 9.81f;
// The dt_years at which the transport constants were calibrated. A landscape
// step represents dt_years, and BOTH processes must scale with it or they are
// on different clocks -- which is what stopped the diffusion coefficient from
// being calibratable at all.
constexpr float kCalibrationYears = 200.0f;

// -------------------------------------------------------------------- fields

// Water carried by one particle, scaled so that drops x volume is the water a
// LANDSCAPE STEP delivers. Erosion scales with volume, so this is what puts
// erosion on the same clock as diffusion. T2 proves splitting the same water
// into a different number of parcels does not change the landscape, so this is
// purely a statement about how much water a step represents -- not a knob.
inline float EffectiveDropVolume(const Params& p) {
  return p.drop_volume * (p.dt_years / kCalibrationYears);
}

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

}  // namespace

namespace pg {

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

}  // namespace pg

// PriorityFlood lives in `namespace pg` (not the anonymous namespace the rest
// of this section uses) because protogen_swe.cpp's SweFillOracle test calls
// it as an oracle across the TU boundary -- see protogen.hpp's declaration
// and its "export deliberately" header note.
namespace pg {

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

}  // namespace pg

namespace {

// Bilinear height at a CONTINUOUS position, cell centres at (i+0.5, j+0.5).
//
// The transport law needs the drop along the path divided by the distance that
// spans -- a directional derivative. Taking it from cell indices instead
// returns the full cell-to-cell drop however short the step was, so a 4 m step
// over 16 m cells reads a gradient 4x too steep and the landscape diverges
// under refinement. Interpolating makes the sample distance and the drop agree
// at any step size, and it still contains h[here], which is what keeps the law
// self-limiting.
inline float SampleField(const std::vector<float>& f, int n, float fx,
                         float fy) {
  const float px = fx - 0.5f, py = fy - 0.5f;
  int x0 = int(std::floor(px)), y0 = int(std::floor(py));
  const float tx = px - float(x0), ty = py - float(y0);
  x0 = std::clamp(x0, 0, n - 2);
  y0 = std::clamp(y0, 0, n - 2);
  const size_t i00 = size_t(y0) * n + x0;
  const float h00 = f[i00], h10 = f[i00 + 1];
  const float h01 = f[i00 + size_t(n)], h11 = f[i00 + size_t(n) + 1];
  return (h00 * (1.f - tx) + h10 * tx) * (1.f - ty) +
         (h01 * (1.f - tx) + h11 * tx) * ty;
}

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

// ------------------------------------------------------------------- descend

// The reference's descend, verbatim. Serial and IN PLACE: particles interact
// through the terrain within a step and that coupling is part of the physics
// (see README).
void Descend(Grid& g, const Params& p, float px, float py, float volume0) {
  const float cell_m = p.world_m / float(p.res);
  const float scale = p.relief_m / cell_m;  // height units per cell width
  // Repose threshold in height units: tan(angle) * cell / relief.
  const float max_diff = std::tan(p.repose_angle_deg * 3.14159265f / 180.0f) *
                         cell_m / p.relief_m;
  V2 pos{px, py}, speed{0.f, 0.f};
  float volume = volume0, sediment = 0.0f;
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
  float total_travel_m = 0.f;  // lifetime is a DISTANCE now, not an iteration count
  size_t last_cell = g.idx(int(px), int(py));

  for (int age = 0; age < p.max_age; ++age) {
    const int x = int(pos.x), y = int(pos.y);
    if (g.oob(x, y)) { g.lost_offmap += double(carried_mass()); return; }
    const size_t here = g.idx(x, y);
    last_cell = here;
    ++g.visits[here];

    if (volume < p.min_vol) {
      // Bed deposit, not a sus injection -- this branch is untouched by the
      // redirect below (it stays unreachable in practice; see the comment
      // above the true terminal exit at the bottom of this function), so it
      // does not count toward injected_sus.
      Deposit(g, here, carried_mass());
      return;
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
      Deposit(g, here, carried_mass());  // bed deposit, not a sus injection
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
    if (p.enable_momentum && len(f) > 0.f && len(speed) > 0.f) {
      // Denominator is the RAW volume EMA alone -- NOT (volume + vol_ema).
      //
      // Steering must not depend on how the water was parcelled: a fat parcel
      // being deflected less than a thin one has no physical basis, and it is
      // the same defect as the gravity/volume removed in Phase 5, inherited
      // from the same reference formula. Measured as a real channel into T2:
      // successive differences 37%/8% with it, 28%/7% without.
      //
      // The saturating role it played is already served by vol_ema, which grows
      // with traffic and is what bounds this force rather than letting it run
      // away.
      const float k = p.momentum_transfer * dot(unit(f), unit(speed)) /
                      std::max(g.vol_ema[here], 1e-6f);
      speed = V2{speed.x + k * f.x, speed.y + k * f.y};
    }
    if (len(speed) <= 0.f) {
      // Gravity and the momentum field cancelled, or the cell is dead flat with
      // no stream to follow. Same rule as every other exit: what the particle
      // is carrying must land somewhere. Returning bare destroyed it, which is
      // the residual that survived the concentration-vs-mass fix -- it scales
      // with how much flat ground a map has, so it barely shows on the steep
      // synthetic fixtures and shows clearly at production scale.
      //
      // And its WATER must convert: drag bringing a parcel to rest in a pit is
      // the dominant way particles end in a closed basin, and this exit was
      // the one T2b caught not converting -- delivery was 0.1% of runoff and
      // parcel-count dependent, both of which vanish once every terrestrial
      // exit hands its rate over.
      //
      // Bed deposit, not a sus injection: this exit scatters over wherever a
      // particle happens to stall (flats, momentum cancellation), not
      // repeatedly onto the same cell the way the terminal exit below does,
      // so it is not the one-cell-pit mechanism Task 3 targets and is left
      // untouched.
      Deposit(g, here, carried_mass());
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
      // Capacity reads the bed one cell ahead along the flow direction (see
      // SampleField for the interpolation and why the gradient is sampled
      // rather than taken from grid indices).
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
  // Reached max_age/max_travel still carrying a load. This is the DOMINANT
  // exit -- volume only decays to 0.61 over 500 steps, so the volume <
  // min_vol branch above is unreachable in practice -- and it used to Deposit
  // the whole load onto the ONE cell it died in, which is what manufactured
  // production's thousands of one-cell pits (measured ~40% of all deposited
  // mass through this single path). Route it into the suspended field
  // instead: SettleSus drains `sus` onto the bed gradually over the following
  // landscape steps (see Params::settle_fraction), turning the spike into a
  // halo instead of leaving it a one-cell artifact.
  g.sus[last_cell] += carried_mass();
  g.injected_sus += double(carried_mass());
}

}  // namespace

namespace pg {

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

// ------------------------------------------- boundary classification -------
//
// See protogen.hpp for the WHY (the seed/grow thresholds and their
// reasoning) -- this is the HOW. Two passes:
//
//   1. SEED: any cell that is both deep (h > kLakeSeedDepthM) and slow
//      (speed < kLakeSeedSpeedMPerS) starts a region.
//   2. GROW: from each unvisited seed, a 4-connected flood fill over the wet
//      component (h > eps_wet), crossing a neighbour only when its water
//      surface is within kLakeSurfaceContinuityM of the SEED's surface --
//      not of the cell it came from (see the fix note below the loop).
//
// DETERMINISM. The seed scan is row-major, but the RESULT does not depend on
// that order: the "surface within tolerance" relation is symmetric, so the
// set of cells reachable from a given set of seeds is a pure function of the
// grid state -- a fixed graph's connected components, not a race. Scan order
// only decides which seed's flood claims a cell first when two seeds' basins
// would otherwise both reach it, and by then both floods agree it belongs to
// the SAME region (their basins are connected through it), so the choice is
// immaterial to the output.
//
// COST. One O(cells) pass to build the surface field, then a flood fill that
// visits each cell at most once (a cell is pushed to the stack only the
// first time it is reached, via the `visited` guard) -- O(cells) total, the
// same order as PriorityFlood above.
std::vector<float> ClassifyBoundaryWater(const Grid& g, const Params& p) {
  const int n = g.n;
  std::vector<float> surface(g.cells);
  for (size_t i = 0; i < g.cells; ++i)
    surface[i] = g.height[i] * p.relief_m + g.h[i];

  // ONE flag, not two: every cell this pass ever marks is both "reached by
  // the flood fill" and "part of a lake region" -- there is no seed or
  // growth site that sets one without the other (see the two write sites
  // below), so a separate `tagged` array alongside `visited` tracked nothing
  // `visited` did not already say. Doubled as the output mask directly.
  std::vector<uint8_t> visited(g.cells, 0);
  std::vector<int32_t> stack;
  stack.reserve(g.cells);
  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};

  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      if (visited[i]) continue;
      const float speed =
          std::sqrt(g.velx[i] * g.velx[i] + g.vely[i] * g.vely[i]);
      const bool is_seed =
          g.h[i] > kLakeSeedDepthM && speed < kLakeSeedSpeedMPerS;
      if (!is_seed) continue;

      // Bound growth against the SEED's surface, not the immediate
      // predecessor's (finding from code review: the old per-HOP check let a
      // near-flat wet reach drift arbitrarily far from the lake one
      // centimetre at a time -- a long, gently-sloped backwater draining a
      // lake never exceeds the tolerance between any two ADJACENT cells, so
      // it walked straight into the mask with nothing to stop it, however
      // far downstream it went. Comparing every candidate to the fixed
      // seed_surface instead makes the bound a TOTAL one, which is also the
      // physically correct reading of "a lake's surface is one equipotential
      // to within tolerance" -- that is a statement about the whole lake
      // against ONE reference level, not a chain of pairwise agreements that
      // can each individually pass while the ends disagree by any amount.
      const float seed_surface = surface[i];
      visited[i] = 1;
      stack.clear();
      stack.push_back(int32_t(i));
      while (!stack.empty()) {
        const size_t cur = size_t(stack.back());
        stack.pop_back();
        const int cx = int(cur % size_t(n)), cy = int(cur / size_t(n));
        for (int k = 0; k < 4; ++k) {
          const int nx = cx + dx[k], ny = cy + dy[k];
          if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
          const size_t j = g.idx(nx, ny);
          if (visited[j]) continue;
          if (!(g.h[j] > p.eps_wet)) continue;  // dry: not part of any lake
          if (std::fabs(surface[j] - seed_surface) > kLakeSurfaceContinuityM)
            continue;  // too far from the seed's level: not this lake
          visited[j] = 1;
          stack.push_back(int32_t(j));
        }
      }
    }
  }

  std::vector<float> out(g.cells, 0.f);
  for (size_t i = 0; i < g.cells; ++i)
    if (visited[i]) out[i] = g.h[i];
  return out;
}

}  // namespace pg

namespace {

// ---------------------------------------------------------- soil production

// Bedrock weathering into regolith:  d(soil)/dt = P0 * exp(-soil / h_star).
//
// The missing source. Soil was a finite stock seeded once and only depleted --
// 4.00 -> 1.19 m with 40% of cells stripped bare, after which hillslopes stop
// evolving because bedrock only yields at bedrock_erodibility. Nothing turned
// rock into regolith.
//
// The exponential is what makes it self-limiting and is not decoration: bare
// rock weathers fastest, and a thick mantle insulates the rock beneath it. That
// is what gives a hillslope a STEADY-STATE thickness under erosion,
// h_eq = h_star * ln(P0 / E), which P4 checks against two erosion rates.
//
// Soil comes FROM bedrock, so bedrock lowers by exactly what soil gains and the
// surface does not move. Real regolith bulks up ~1.5x as it weathers; 1:1 is
// the conservative choice and avoids inventing volume. Since `height` is the
// surface and bedrock is implicit (height - soil), leaving height untouched
// while raising soil IS the conversion.
//
// ============================ LANDMINE, READ BEFORE MOVING THIS =============
// THIS PASS IS PHASE-0-ONLY, AND WIRING IT INTO THE PHASE-1 CYCLE LOOP AT
// morfac > 1 IS A CORRECTNESS BUG, NOT A CONFIGURATION CHOICE.
//
// Phase 1's `SedExchange` (tools/protogen/protogen_swe.cpp) applies MORFAC to
// the erosion demand BEFORE the soil/bedrock split, i.e. it asks the yield law
// once for M cycles' worth of demand instead of asking it M times. That is
// exact -- but only because the soil-first yield law is exactly ADDITIVE over
// sequential demands on a MONOTONICALLY DEPLETING column:
//
//     yield(D1) then yield(D2)  ==  yield(D1 + D2)      (soil only shrinks)
//
// THIS PASS BREAKS THAT PRECONDITION. Weathering puts cover BACK between fluid
// intervals, so across M real intervals the soil/bedrock boundary moves in both
// directions and where it ends up depends on the interleaving -- which a single
// yield(M*D) call cannot know. The error is worst exactly at the transition,
// which is the boundary deciding whether a slope is armoured rock or strippable
// regolith, so it is not a rounding-scale concern.
//
// `RunSweCycles` carries a tripwire for precisely this (it aborts on
// `enable_soil_production && morfac > 1`, with the reason spelled out). The fix,
// when someone genuinely needs weathering under acceleration, is to SUB-STEP
// `SedExchange` across the mantle -- not to widen the guard.
// ============================================================================
void ProduceSoil(Grid& g, const Params& p) {
  if (!p.enable_soil_production) return;
  const float h_star = std::max(p.soil_efold_m, 1e-6f);
  const float dt = p.dt_years;
  badlands::ParallelFor(size_t(p.res), [&](size_t yy) {
    const size_t base = yy * size_t(p.res);
    for (int x = 0; x < p.res; ++x) {
      const size_t i = base + x;
      const float soil_m = g.soil[i] * p.relief_m;
      // Production, and the synthetic constant removal P4 uses to hold an
      // erosion rate fixed while the balance settles.
      const float d_m = (p.soil_production_m_per_yr *
                             std::exp(-soil_m / h_star) -
                         p.soil_strip_m_per_yr) * dt;
      g.soil[i] = std::max(0.f, g.soil[i] + d_m / p.relief_m);
    }
  });
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

// -------------------------------------------------------------- sus settling

// Drains `sus`, the suspended-sediment field the particle walk's terminal
// exit injects into instead of dumping its whole load onto the one cell it
// died in (see Descend's post-loop comment). GATHER-ONLY Jacobi, like
// Diffuse: every cell reads only the FRONT buffer (`sus`) and writes only its
// own back-buffer slot (`sus_b`) plus its own soil/height through Deposit,
// which touches cell-local state alone -- it reads nothing and writes
// g.soil[c]/g.height[c] for the single c it is given -- so calling it once
// per cell inside the parallel body is safe: distinct cells never share a
// write target, and nothing here mutates `sus` (the buffer every cell is
// reading) until the swap at the very end.
//
// Two things happen to a cell's suspended load each step, in this order:
//   1. `settle_fraction` of it joins the bed as soil, via Deposit -- loose
//      material by definition, exactly like a particle's own deposit.
//   2. `sus_diffusion` of what is left spreads equally to the four
//      orthogonal neighbours; the rest stays put for next step's settling.
// Unlike Diffuse's flux-balance form (driven by height DIFFERENCES, so it
// only smooths existing relief), this is an isotropic push: every cell sheds
// the same FRACTION of its own load regardless of what its neighbours hold,
// because `sus` has no slope of its own to follow -- an isotropic spread is
// exactly the mechanism that turns a one-cell spike into a halo.
//
// Border cells lose part of their diffusion share off the edge of the grid.
// That is real transport leaving the map, so it is booked to `lost_offmap`
// exactly like a particle that walks off the border -- the SAME ledger, so
// the mass print's closure does not need a separate term for it.
void SettleSus(Grid& g, const Params& p) {
  const int n = g.n;
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};

  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float remainder_i = g.sus[i] * (1.0f - p.settle_fraction);
      float next = remainder_i * (1.0f - p.sus_diffusion);
      for (int m = 0; m < 4; ++m) {
        const int ax = x + dx4[m], ay = y + dy4[m];
        if (ax < 0 || ay < 0 || ax >= n || ay >= n) continue;  // exits below
        const size_t j = g.idx(ax, ay);
        const float remainder_j = g.sus[j] * (1.0f - p.settle_fraction);
        next += remainder_j * p.sus_diffusion * 0.25f;
      }
      g.sus_b[i] = next;
      // Cell-local: this thread owns (x, y) alone, and Deposit(g, i, ...)
      // touches only g.soil[i]/g.height[i] -- see the function comment.
      Deposit(g, i, g.sus[i] * p.settle_fraction);
    }
  });

  // Border loss, from the FRONT buffer alone so it does not depend on the
  // parallel pass above having reached any particular cell -- computed once,
  // serially, over just the O(n) perimeter rather than the O(n^2) interior.
  double lost = 0.0;
  auto missing = [&](int x, int y) {
    return (x == 0) + (x == n - 1) + (y == 0) + (y == n - 1);
  };
  auto accumulate = [&](int x, int y) {
    const size_t i = g.idx(x, y);
    const float remainder_i = g.sus[i] * (1.0f - p.settle_fraction);
    lost += double(remainder_i) * double(p.sus_diffusion) * 0.25 *
            double(missing(x, y));
  };
  for (int x = 0; x < n; ++x) { accumulate(x, 0); accumulate(x, n - 1); }
  for (int y = 1; y < n - 1; ++y) { accumulate(0, y); accumulate(n - 1, y); }
  g.lost_offmap += lost;

  g.sus.swap(g.sus_b);
}

// -------------------------------------------------------------------- output

// One headerless .f32 raster, `f[i] * sc` per cell. Shared by Dump() and
// DumpPhase1() below so the on-disk form (row-major, no header, real units
// after scaling) has exactly one place that writes it.
void WriteF32Raster(const Params& p, const std::string& tag, const char* name,
                    const std::vector<float>& f, float sc) {
  std::vector<float> tmp(f.size());
  for (size_t i = 0; i < f.size(); ++i) tmp[i] = f[i] * sc;
  const std::string path = p.out + "/" + tag + "-" + name + ".f32";
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    std::fprintf(stderr, "protogen: cannot write %s\n", path.c_str());
    return;
  }
  std::fwrite(tmp.data(), sizeof(float), tmp.size(), fp);
  std::fclose(fp);
}

// Phase-0's raster set: height/water/discharge/Q/soil. Used for the initial
// dump, RunSim's own periodic snapshots, and (unconditionally, before phase
// 1 gets a chance to run) the phase-0-finished state -- see main()'s own
// comment for why the phase-1 branch does not replace this call, only
// follows it.
void Dump(const Grid& g, const Params& p, const std::string& tag) {
  WriteF32Raster(p, tag, "height", g.height, p.relief_m);  // metres
  // Boundary classification (protogen.hpp/ClassifyBoundaryWater) is the
  // ONLY place a lake/river distinction exists. For a phase-0-only grid `h`
  // is identically 0 -- it is a phase-1 field the particle walk never
  // touches -- so this classifies to no seeds and grows nothing: BYTE
  // IDENTICAL to the old hardcoded-zero write, which is what keeps a
  // `--cycles 0` run's `water.f32` shaped exactly like it always was.
  WriteF32Raster(p, tag, "water", ClassifyBoundaryWater(g, p), 1.0f);
  WriteF32Raster(p, tag, "discharge", g.discharge, 1.0f);
  WriteF32Raster(p, tag, "Q", g.Qm3s, 1.0f);                // m^3/s
  WriteF32Raster(p, tag, "soil", g.soil, p.relief_m);       // metres of cover
}

// Phase-1's raster set (Task 7): height/soil again (self-contained, like
// every phase-0 tag), the lake-only water classification, depth (raw `h`,
// wet OR dry, channel OR lake -- unlike `water` this is deliberately NOT
// classification-masked, since a depth raster's whole point is showing the
// wet state honestly), speed magnitude, and the suspended load. NOT
// discharge/Q: those are phase-0 particle-EMA fields that phase 1 never
// updates, so writing them under a phase-1 tag would dump stale phase-0
// state under a new name instead of honestly having no opinion.
//
// Used both for the `--snapshot-every`-cadence periodic dumps during
// RunSweCycles and for the final one at the end of the run -- deliberately
// the SAME function and SAME raster set for both, rather than a leaner
// mid-run set plus extra rasters only at the end: a periodic snapshot is
// only useful for diagnosing a long run if every one of them is already
// everything --extract-rivers and show.py need, not almost.
void DumpPhase1(const Grid& g, const Params& p, const std::string& tag) {
  WriteF32Raster(p, tag, "height", g.height, p.relief_m);
  WriteF32Raster(p, tag, "water", ClassifyBoundaryWater(g, p), 1.0f);
  WriteF32Raster(p, tag, "depth", g.h, 1.0f);
  std::vector<float> speed(g.cells);
  for (size_t i = 0; i < g.cells; ++i)
    speed[i] = std::sqrt(g.velx[i] * g.velx[i] + g.vely[i] * g.vely[i]);
  WriteF32Raster(p, tag, "vel", speed, 1.0f);
  WriteF32Raster(p, tag, "sus", g.sus, p.relief_m);
  WriteF32Raster(p, tag, "soil", g.soil, p.relief_m);
}

}  // namespace

namespace pg {

// Runs the whole simulation. Extracted from main so the sanity tests exercise
// the REAL loop rather than a copy that can drift from it.
void RunSim(const Params& p, Grid& g, SimStats& st, bool verbose) {
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
  using clk = std::chrono::steady_clock;
  double t_drops = 0, t_grid = 0, t_settle = 0;
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
    for (int d = 0; d < p.drops; ++d)
      Descend(g, p, sx[d], sy[d], EffectiveDropVolume(p));

    auto tC = clk::now(); t_drops += secs(tA, tC);
    // EMA, double buffered: one writer per cell, so no atomics. Averaging is
    // what lets a channel persist between steps and attract later drops.
    // With NO particles there is nothing to average, and the EMA would decay an
    // INJECTED discharge field to zero. Holding the fields as given is what lets
    // a water test supply its own inflow instead of generating it with the
    // particle pass -- which is how T12 came to depend on particle stepping.
    if (p.drops > 0) {
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
    }

    ProduceSoil(g, p);
    Diffuse(g, p, diff_demand, diff_limit, diff_delta);

    auto tD = clk::now(); t_grid += secs(tC, tD);

    SettleSus(g, p);

    auto tE = clk::now(); t_settle += secs(tD, tE);

    // Guarded the same way phase 1's cadence is (main(), `std::max(1, ...)`):
    // `--snapshot-every 0` must throttle to "every step", not SIGFPE on the
    // modulo -- integer division/modulo by zero is undefined behaviour, and
    // on this hardware that is a hardware trap, not a graceful default.
    if (verbose && (step % std::max(1, p.snapshot_every) == 0 || step == p.steps)) {
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
      double sus_sum = 0.0;
      for (float s : g.sus) sus_sum += double(s);
      std::printf("  step %4d/%d  relief %8.1f m\n"
                  "      mass: sum(h) %.4e  sus %.4e  injected_sus %.4e  offmap %.4e\n"
                  "      visits/cell: touched %zu  mean %.2f  p50 %u  p99 %u  max %u\n",
                  step, p.steps, (hi - lo) * p.relief_m, hsum, sus_sum,
                  g.injected_sus, g.lost_offmap, v.size(),
                  v.empty() ? 0.0 : tot / double(v.size()),
                  v.empty() ? 0u : v[v.size() / 2],
                  v.empty() ? 0u : v[size_t(v.size() * 0.99)],
                  v.empty() ? 0u : v.back());
      std::fflush(stdout);
    }
  }

  // TERMINAL FLUSH (code review finding). SettleSus only drains a FRACTION
  // of a cell's suspended load each call (`settle_fraction`, the rest
  // diffusing to neighbours to settle over FUTURE steps -- see its own
  // comment) -- fine while the sim keeps iterating, but RunSim has no next
  // step, so a finite run always ends with some nonzero remainder still
  // parked in `sus` (~0.13% of deposited mass, measured on a
  // production-scale run). The IN-MEMORY ledger already counts that as
  // real, unsettled mass (MassConservation's SumSus adds `sus` to `height`
  // on the "still on the map" side), but the ON-DISK artifacts --
  // height/soil rasters, and world.txt's soil quantiles, both built from
  // `g.height`/`g.soil` alone -- never see it, so whatever is still in
  // `sus` when the run ends silently never reaches disk.
  //
  // A DIRECT final deposit, not a bounded number of extra SettleSus passes:
  // SettleSus's gradual drain-and-diffuse behaviour only means something
  // because there ARE future steps for the remainder to keep settling over.
  // There are none here, so "gradual" buys nothing, and a fixed pass count
  // would still leave an arbitrary nonzero remainder -- SettleSus decays
  // `sus` geometrically (by `1 - settle_fraction` each call), which never
  // reaches exactly zero in finitely many passes. That residual is exactly
  // what this fix exists to close, so leaving one by construction would
  // defeat it. Depositing the WHOLE remaining load in place instead --
  // one Deposit per cell, exactly where its `sus` currently sits, no
  // diffusion -- is exact in a single pass (nothing left over to argue a
  // pass count for) and reuses the same conservative Deposit() every other
  // erosion/settling path in this file already goes through (soil-first,
  // and a no-op below its own `amount <= 0.f` guard).
  //
  // Cell-local, like SettleSus's own Deposit call (see its header comment
  // for the argument in full): each thread owns index `i` alone, so this is
  // safe inside a parallel body.
  badlands::ParallelFor(size_t(p.res), [&](size_t yy) {
    const size_t base = yy * size_t(p.res);
    for (int x = 0; x < p.res; ++x) {
      const size_t i = base + x;
      Deposit(g, i, g.sus[i]);
      g.sus[i] = 0.0f;
    }
  });
  if (verbose && p.steps > 0) {
    // Re-dump the FINAL tag so the on-disk snapshot reflects the flush too
    // -- the in-loop Dump() above already wrote this exact tag one step
    // ago, before `sus` was zeroed; overwriting it here is what makes the
    // artifact match the in-memory ledger MassConservation already checks.
    char tag[64];
    std::snprintf(tag, sizeof(tag), "%04d-step", p.steps);
    Dump(g, p, tag);
  }

  st.t_drops = t_drops; st.t_grid = t_grid; st.t_settle = t_settle;
}

}  // namespace pg

namespace {

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
  // Phase-1 provenance (Task 7), mirroring CoarseManifest's own three new
  // fields exactly -- 0/0/0 means "phase 1 did not run" on this input, which
  // is what BuildInputsFromGrid's default leaves it at for the unconditional
  // phase-0 WriteWorldArtifacts call (see main()).
  float morfac = 0.0f;
  int cycles = 0;
  int substeps = 0;
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
  man.morfac = in.morfac;
  man.cycles = in.cycles;
  man.substeps = in.substeps;

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

// Code review finding: an aborted phase-1 run must not silently write
// world.txt/rasters off a POISONED grid (a NaN/Inf tripwire is exactly what
// aborted it) -- see main()'s abort branch for the write side of this. A
// plain serial scan is fine here: this only ever runs once, on the abort
// path, never in a hot loop, so it does not need DeterministicMaxDepth's
// chunked-parallel-reduction machinery -- it only needs the same
// `std::isfinite` check that machinery uses, over EVERY field
// BuildInputsFromGrid/DumpPhase1 below actually reads: `height` and `soil`
// feed the on-disk bed/cover rasters and SoilCutoffs' quantile sort (which
// has no defined behaviour for a NaN input); `h`/`velx`/`vely` feed
// ClassifyBoundaryWater's depth/speed test, which is what the lake/river
// split and `water.f32` are built from; `sus` is DumpPhase1's own
// `-sus.f32` raster (round-review finding: an earlier version of this check
// omitted it, so a NaN confined to `sus` alone classified as FINITE and
// landed on disk under a name indistinguishable from a good snapshot --
// exactly the failure mode this whole function exists to prevent).
bool GridFinite(const Grid& g) {
  for (size_t i = 0; i < g.cells; ++i) {
    if (!std::isfinite(g.height[i]) || !std::isfinite(g.soil[i]) ||
        !std::isfinite(g.h[i]) || !std::isfinite(g.velx[i]) ||
        !std::isfinite(g.vely[i]) || !std::isfinite(g.sus[i]))
      return false;
  }
  return true;
}

// `cycles_run`: 0 (the default) for the unconditional phase-0 call in
// main() -- leaves the manifest's phase-1 provenance at 0/0/0, i.e. "did not
// run". The phase-1 final call passes the actual completed cycle count, so
// the provenance fields name what really happened even if a tripwire cut
// the run short of `p.cycles`.
WorldArtifactInputs BuildInputsFromGrid(const Grid& g, const Params& p,
                                        int cycles_run = 0) {
  WorldArtifactInputs in;
  in.res = g.n;
  in.world_m = p.world_m;
  in.runoff_m_per_yr = p.runoff_m_per_yr;
  in.seed = p.seed;
  in.steps = p.steps;
  in.height_m = ToField2D(g.height, g.n, p.relief_m);
  // Boundary classification (see ClassifyBoundaryWater's header comment):
  // for a phase-0-only grid `h` is identically 0, so this is all zero --
  // the same 0-lake case WriteWorldArtifacts/SoilCutoffs/river extraction
  // already handled when `water_m` was a hardcoded zero field.
  in.water_m = mg::Field2D<float>(g.n, g.n);
  in.water_m.data = ClassifyBoundaryWater(g, p);
  in.soil_m = ToField2D(g.soil, g.n, p.relief_m);
  if (cycles_run > 0) {
    in.morfac = p.morfac;
    in.cycles = cycles_run;
    in.substeps = p.swe_substeps;
  }
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
// Reads the FINAL snapshot. Which tag that is depends on whether phase 1
// ran (Task 7): `man->cycles > 0` means the true final state is a
// DumpPhase1 tag at the completed cycle count (main() names its phase-1
// snapshots "%04d-cycle" and always lands one exactly there, whatever
// `cycles % snapshot_every` is -- see main()'s own comment); otherwise it is
// phase-0's own final tag, which RunSim always dumps at step == p.steps
// regardless of --snapshot-every.
int RunExtractRivers(const std::string& dir) {
  std::string err;
  const auto man = mg::load_coarse_manifest(dir, &err);
  if (!man) {
    std::fprintf(stderr, "protogen: --extract-rivers: %s\n", err.c_str());
    return 1;
  }

  char tag[64];
  if (man->cycles > 0)
    std::snprintf(tag, sizeof(tag), "%04d-cycle", man->cycles);
  else
    std::snprintf(tag, sizeof(tag), "%04d-step", man->steps);
  const std::string base = dir + "/" + std::string(tag) + "-";

  WorldArtifactInputs in;
  in.res = man->resolution;
  in.world_m = man->world_size_m;
  in.runoff_m_per_yr = man->runoff_m_per_yr;
  in.seed = man->seed;
  in.steps = man->steps;
  // Carry the provenance straight through so re-running --extract-rivers on
  // its own output does not silently drop it from world.txt.
  in.morfac = man->morfac;
  in.cycles = man->cycles;
  in.substeps = man->substeps;

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

// ------------------------------------------------- run diagnostics (Task 7)
//
// PRINTED, NOT ASSERTED: these are about what a PRODUCTION run's landscape
// looks like, not a pass-or-fail statement about the sim -- a run with
// plenty of depressions or off-map drainage is not necessarily wrong, it is
// information Task 8's full-run validation needs a baseline for.

// 8-neighbour local minima on the final bed: a cheap, purely LOCAL proxy for
// "how many depressions does this landscape have" -- not a re-run of
// PriorityFlood's actual basin resolution (which would also need to decide
// what counts as a real depression vs. float noise on an otherwise flat
// reach), just a count of cells no immediate neighbour undercuts. Border
// cells are excluded from being candidates, matching this file's other
// border convention (`Grid::oob`): the border is open/base level by
// definition, never a depression.
int CountLocalMinima8(const Grid& g) {
  const int n = g.n;
  static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
  int count = 0;
  for (int y = 1; y < n - 1; ++y) {
    for (int x = 1; x < n - 1; ++x) {
      const float z = g.height[g.idx(x, y)];
      bool is_min = true;
      for (int k = 0; k < 8 && is_min; ++k)
        if (g.height[g.idx(x + dx[k], y + dy[k])] < z) is_min = false;
      if (is_min) ++count;
    }
  }
  return count;
}

// Fraction of cells whose D8 steepest-descent path on the FINAL bed reaches
// the map border, vs. one that dead-ends at a local minimum (a depression,
// or a cycle across a float-flat plateau). One receiver per cell (the
// lowest of its 8 neighbours, or itself if none is lower), then every
// chain is walked with memoization -- a cell's drains-off-map/stuck verdict
// is resolved exactly once and every subsequent walk that reaches an
// already-resolved cell reads the cached answer instead of re-walking --
// so the whole thing costs O(cells) amortized, the same order as
// PriorityFlood, despite naively looking like O(cells * path length).
double FractionDrainsOffMap(const Grid& g) {
  const int n = g.n;
  static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

  std::vector<int32_t> receiver(g.cells);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      if (x == 0 || y == 0 || x == n - 1 || y == n - 1) {
        receiver[i] = -1;  // border: already off-map
        continue;
      }
      float best = g.height[i];
      int32_t best_j = int32_t(i);  // no lower neighbour -> a sink (itself)
      for (int k = 0; k < 8; ++k) {
        const size_t j = g.idx(x + dx[k], y + dy[k]);
        if (g.height[j] < best) { best = g.height[j]; best_j = int32_t(j); }
      }
      receiver[i] = best_j;
    }
  }

  // status: 0 unresolved, 1 drains to the border, 2 stuck (sink or cycle).
  std::vector<uint8_t> status(g.cells, 0);
  std::vector<int32_t> stamp(g.cells, -1);  // which walk last touched a cell
  std::vector<int32_t> path;
  path.reserve(256);
  int32_t walk_id = 0;
  size_t drains = 0;

  for (size_t start = 0; start < g.cells; ++start) {
    if (status[start] == 0) {
      // Not yet resolved by any earlier walk (neither as a `start` of its
      // own nor as a waypoint on someone else's path) -- walk it.
      path.clear();
      int32_t cur = int32_t(start);
      ++walk_id;
      uint8_t result = 0;
      while (true) {
        if (status[size_t(cur)] != 0) { result = status[size_t(cur)]; break; }
        if (stamp[size_t(cur)] == walk_id) { result = 2; break; }  // cycle
        stamp[size_t(cur)] = walk_id;
        path.push_back(cur);
        const int32_t r = receiver[size_t(cur)];
        if (r == -1) { result = 1; break; }   // reached the border
        if (r == cur) { result = 2; break; }  // local minimum
        cur = r;
      }
      for (int32_t c : path) status[size_t(c)] = result;
    }
    // Counted EXACTLY HERE, once per cell, regardless of whether `start`'s
    // status was just set by its own walk above or was already set by an
    // EARLIER walk that happened to pass through it on the way to its own
    // border/sink. The previous version also incremented `drains` by
    // `path.size()` inside the walk itself, which double-counted every
    // waypoint cell the very first time the outer loop's monotonic scan
    // later reached it as a `start` in its own right -- measured to inflate
    // the reported fraction past 100% on a 128x128 fixture.
    if (status[start] == 1) ++drains;
  }
  return g.cells ? double(drains) / double(g.cells) : 0.0;
}

// Depression count, off-map drainage fraction, the two advection-fixer
// diagnostics carried on `Grid` since Task 6, and (final-review finding 1)
// the bed-quantization discard counters. Called TWICE by a `--cycles >
// 0` run that completes -- once on the phase-0-finished grid (right after
// the first WriteWorldArtifacts), once on the phase-1-finished one -- so the
// two prints together are the "before vs after phase 1" comparison Task 8's
// full-run validation is built around; a `--cycles 0` run gets only the
// first. The advection-fixer and quantization numbers are naturally 0 on the
// phase-0-only print (phase 1 has not run yet). Matters most for a
// MORFAC-accelerated production run, where a dry-start or otherwise
// ill-posed advection step can quietly destroy suspended load --
// `swe_sed_advect_fix_max`/`swe_sed_advect_fix_residual_m3` are how that
// would show up here rather than only in the (rarer) mass-audit tripwire --
// and where a small `--relief` widens the quantization quantum against a
// fixed bed-delta magnitude, which the discard counters make visible instead
// of silent.
void PrintRunDiagnostics(const Grid& g, const char* phase_label) {
  const int depressions = CountLocalMinima8(g);
  const double off_map_frac = FractionDrainsOffMap(g);
  std::printf(
      "protogen: %s diagnostics\n"
      "  depressions (8-neighbour local minima on the final bed): %d\n"
      "  cells draining off-map (D8 on the final bed): %.2f%%\n"
      "  advection fixer: worst |factor-1| %.4e, unplaced residual %.4e m3\n"
      "  bed quantization discards: erosion %.0f cells / %.4e m solid, "
      "deposition %.0f cells / %.4e m solid\n",
      phase_label, depressions, 100.0 * off_map_frac,
      g.swe_sed_advect_fix_max, g.swe_sed_advect_fix_residual_m3,
      g.swe_sed_quant_discard_erosion_count,
      g.swe_sed_quant_discard_erosion_m,
      g.swe_sed_quant_discard_deposition_count,
      g.swe_sed_quant_discard_deposition_m);
}

// ------------------------------------------------- perf report (Task 7) ----
//
// One row per timed pass: total wall time, mean per CALL (a substep for the
// three fluid passes, a cycle for everything phase-1 beyond that, a step
// for phase-0's rows -- the natural unit for each, see the header comment
// on SimStats), and throughput. "cell-updates/sec" for every full-grid
// Jacobi pass; `drops` is the one exception (a per-PARTICLE pass, not
// per-cell), labelled `particle-steps/sec` instead rather than reporting a
// cell-updates number that would not mean what it says.
void AddPerfRow(std::string& out, const char* name, double total_s,
                double calls, double units_per_call, const char* call_unit,
                const char* throughput_unit) {
  char buf[220];
  if (calls > 0.0 && total_s > 0.0) {
    const double mean = total_s / calls;
    const double throughput = (units_per_call * calls) / total_s;
    std::snprintf(buf, sizeof(buf),
                  "  %-14s total %10.3f s   mean %10.3e s/%-8s   "
                  "%12.4e %s\n",
                  name, total_s, mean, call_unit, throughput,
                  throughput_unit);
  } else {
    std::snprintf(buf, sizeof(buf), "  %-14s total %10.3f s   (no calls)\n",
                  name, total_s);
  }
  out += buf;
}

// Builds the printed-AND-perf.txt report. `cycles_run` is how many phase-1
// cycles actually completed (0 if phase 1 never ran, or if it aborted
// before finishing a single one) -- separate from `p.cycles`, the REQUESTED
// count, so an aborted run's throughput numbers are honest about what
// actually executed rather than dividing by a count that never completed.
std::string BuildPerfReport(const Grid& g, const Params& p,
                            const SimStats& st, int cycles_run) {
  std::string out = "protogen: perf\n";
  const double cells = double(g.cells);
  AddPerfRow(out, "drops", st.t_drops, double(p.steps), double(p.drops),
             "step", "particle-steps/s");
  AddPerfRow(out, "grid", st.t_grid, double(p.steps), cells, "step",
             "cell-upd/s");
  AddPerfRow(out, "settle", st.t_settle, double(p.steps), cells, "step",
             "cell-upd/s");
  if (p.cycles > 0) {
    AddPerfRow(out, "warm_start", st.t_swe_warm_start, 1.0, cells, "call",
               "cell-upd/s");
    const double fluid_calls = double(cycles_run) * double(p.swe_substeps);
    AddPerfRow(out, "swe_flux", st.t_swe_flux, fluid_calls, cells, "substep",
               "cell-upd/s");
    AddPerfRow(out, "swe_depth", st.t_swe_depth, fluid_calls, cells,
               "substep", "cell-upd/s");
    AddPerfRow(out, "swe_velocity", st.t_swe_velocity, fluid_calls, cells,
               "substep", "cell-upd/s");
    AddPerfRow(out, "cfl_reduce", st.t_swe_reduce, double(cycles_run), cells,
               "cycle", "cell-upd/s");
    // Morpho hook: 0 calls (not `cycles_run`) when morfac == 0 -- the hook
    // is SKIPPED entirely then (see RunSweCycles), not run to a no-op, so
    // the timers genuinely have nothing behind them.
    const double morpho_calls = p.morfac > 0.f ? double(cycles_run) : 0.0;
    AddPerfRow(out, "sed_exchange", st.t_swe_sed_exchange, morpho_calls,
               cells, "cycle", "cell-upd/s");
    AddPerfRow(out, "sed_advect", st.t_swe_sed_advect, morpho_calls, cells,
               "cycle", "cell-upd/s");
    AddPerfRow(out, "talus_flux", st.t_swe_talus_flux, morpho_calls, cells,
               "cycle", "cell-upd/s");
    AddPerfRow(out, "talus_apply", st.t_swe_talus_apply, morpho_calls, cells,
               "cycle", "cell-upd/s");
  }
  return out;
}

// Prints the report AND writes it to `<out>/perf.txt` -- world.txt stays a
// pure geometry/provenance contract, so timings live in their own file
// rather than growing CoarseManifest with something no downstream consumer
// (mapview, the patch providers) has any use for.
void WritePerfReport(const Grid& g, const Params& p, const SimStats& st,
                     int cycles_run) {
  const std::string report = BuildPerfReport(g, p, st, cycles_run);
  std::printf("%s", report.c_str());
  const std::string path = p.out + "/perf.txt";
  FILE* fp = std::fopen(path.c_str(), "w");
  if (!fp) {
    std::fprintf(stderr, "protogen: cannot write %s\n", path.c_str());
    return;
  }
  std::fwrite(report.data(), 1, report.size(), fp);
  std::fclose(fp);
}

}  // namespace

namespace pg {

// Handles a phase-1 abort's OUTPUT-BOUNDARY consequences (code review
// finding): decides whether to rebuild world.txt/rasters off the live grid
// or write an ABORTED marker instead, and does it. Extracted from main() so
// a test can drive it directly on a tiny fixture -- the same reason
// RunSim/RunSweCycles are their own functions rather than inline code in
// main() (see RunSim's own comment).
//
// Leaving this branch empty (the pre-fix behaviour) is what let an aborted
// run leave its directory self-CONTRADICTORY: completed "%04d-cycle"
// snapshots past what world.txt claims, since world.txt's `cycles` field is
// otherwise only ever advanced by the SUCCESS path (main()'s own
// WriteWorldArtifacts call once the whole requested cycle count completes).
// FindLatestTag (src/mapgen/coarse_world_patch_source.cpp) and
// RunExtractRivers (this file) both trust that field to pick the right tag;
// a stale one makes either silently render the wrong bed with no
// diagnostic.
//
// Two cases, handled differently on purpose:
//
//   FINITE (a dt-floor trip, a sediment-mass-audit trip, or the
//   soil-production config guard): `g` is a perfectly good, physically
//   sensible bed -- the run merely cannot CONTINUE (dt collapsed below the
//   floor, the ledger stopped balancing, or a config combination the sim
//   refuses to run), which says nothing against the state already reached.
//   Treated exactly like a normal batch boundary that happens to be the
//   last one: dump a "%04d-cycle" snapshot and rewrite world.txt off it, at
//   `cycles_run` (the caller's own conservative count of trustworthy
//   cycles -- see SweRunResult::aborted_cycle's header comment). A snapshot
//   at that exact tag was never written by the batching loop (aborting a
//   batch skips its DumpPhase1 call), so there is nothing to collide with;
//   if `cycles_run` happens to equal an earlier batch's own tag (the guard
//   fires before this call's first cycle), this re-dumps the identical,
//   unchanged state under the same name -- harmless.
//
//   NON-FINITE (a NaN/Inf tripwire): `g` IS the poisoned state that
//   aborted the run, by definition. Writing height/soil/water rasters off
//   it would put NaN/Inf on disk under a name indistinguishable from a good
//   snapshot -- and SoilCutoffs' quantile sort has no defined ordering for
//   a NaN input either. So world.txt is left exactly as the last
//   TRUSTWORTHY write (phase 0, or an earlier successful batch) left it,
//   and a plain marker file records what happened instead: a human or a
//   downstream consumer sees unambiguously that this directory's newest
//   work never reached disk, and why, rather than inferring it from an
//   unusually stale `cycles` field.
//
// Returns false only on an I/O failure while trying to make the directory
// self-consistent (never because of the abort itself) -- main() propagates
// that as an exit code, same as every other WriteWorldArtifacts call site.
bool HandlePhase1Abort(const Grid& g, const Params& p, int cycles_run,
                       const std::string& abort_reason) {
  if (GridFinite(g)) {
    char tag[64];
    std::snprintf(tag, sizeof(tag), "%04d-cycle", cycles_run);
    DumpPhase1(g, p, tag);
    if (!WriteWorldArtifacts(BuildInputsFromGrid(g, p, cycles_run), p.out))
      return false;
    std::printf("protogen: abort state is finite -- wrote snapshot '%s' "
                "and updated world.txt at the aborted cycle count\n",
                tag);
    return true;
  }
  const std::string marker_path = p.out + "/ABORTED";
  FILE* fp = std::fopen(marker_path.c_str(), "w");
  if (!fp) {
    std::fprintf(stderr, "protogen: cannot write %s\n", marker_path.c_str());
    return false;
  }
  std::fprintf(fp,
               "protogen: phase 1 aborted at cycle %d of %d requested with "
               "NON-FINITE state -- world.txt was left at its last "
               "trustworthy provenance; no rasters were written from this "
               "cycle.\nreason: %s\n",
               cycles_run, p.cycles, abort_reason.c_str());
  std::fclose(fp);
  return true;
}

}  // namespace pg

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
    else if (a == "--entrainment") p.entrainment = std::stof(nxt());
    else if (a == "--adaptation-length")
      p.adaptation_length_m = std::stof(nxt());
    else if (a == "--momentum") p.momentum_transfer = std::stof(nxt());
    else if (a == "--amp-low") p.amp_low_m = std::stof(nxt());
    else if (a == "--lrate") p.lrate = std::stof(nxt());
    else if (a == "--repose") p.repose_angle_deg = std::stof(nxt());
    else if (a == "--bedrock-erodibility") p.bedrock_erodibility = std::stof(nxt());
    else if (a == "--initial-soil") p.initial_soil_m = std::stof(nxt());
    else if (a == "--cascade-settling") p.settling = std::stof(nxt());
    else if (a == "--settle-fraction") p.settle_fraction = std::stof(nxt());
    else if (a == "--sus-diffusion") p.sus_diffusion = std::stof(nxt());
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
    else if (a == "--dt") p.dt_years = std::stof(nxt());
    else if (a == "--source-x") p.source_x_frac = std::stof(nxt());
    else if (a == "--source-y") p.source_y_frac = std::stof(nxt());
    else if (a == "--source-jitter") p.source_jitter_cells = std::stof(nxt());
    else if (a == "--bowl-rim") p.bowl_rim_m = std::stof(nxt());
    else if (a == "--bowl-well") p.bowl_well_m = std::stof(nxt());
    // Phase-1 SWE fluid knobs (protogen_swe.cpp), consumed by the
    // RunSweCycles call below once `p.cycles > 0`.
    else if (a == "--swe-substeps") p.swe_substeps = std::stoi(nxt());
    else if (a == "--cfl") p.cfl_number = std::stof(nxt());
    else if (a == "--swe-manning-n") p.swe_manning_n = std::stof(nxt());
    else if (a == "--eps-wet") p.eps_wet = std::stof(nxt());
    else if (a == "--dt-floor") p.dt_floor_s = std::stof(nxt());
    // Phase-1 morphodynamics (Task 6). `--morfac` is the landform-time
    // accelerator; the other three are the transport law's own knobs.
    else if (a == "--morfac") p.morfac = std::stof(nxt());
    else if (a == "--kc") p.capacity_Kc_s = std::stof(nxt());
    else if (a == "--settling-velocity")
      p.sus_settling_velocity_m_per_s = std::stof(nxt());
    else if (a == "--transverse-slope")
      p.transverse_slope_coeff = std::stof(nxt());
    else if (a == "--talus-relaxation")
      p.talus_relaxation_per_yr = std::stof(nxt());
    // Phase-1 cycle count (Task 7): 0 (default) leaves phase 1 off. See
    // Params::cycles's own comment.
    else if (a == "--cycles") p.cycles = std::stoi(nxt());
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

  Grid g(p.res);
  InitTerrain(g, p);

  const unsigned workers = std::max(1u, badlands::GetWorkerThreadCount());

  std::printf("protogen: %dx%d cells, %.0f m world, %.1f m cells, %d workers\n"
              "  relief %.0f m, %d steps x %d drops\n"
              "  runoff %.2f m/yr, evaporation %.2f m/yr\n"
              "  settle %.3f/step, sus-diffusion %.3f\n"
              "  swe: %d substeps, CFL %.2f, manning-n %.3f, eps-wet %.4f m, "
              "dt-floor %.1e s\n"
              "  morpho: morfac %.0f, Kc %.4g s, settling %.1e m/s, "
              "transverse-slope %.2f, repose %.0f deg, talus %.3g /yr\n"
              "  phase 1: --cycles %d%s\n",
              p.res, p.res, p.world_m, cell_m, workers, p.relief_m, p.steps,
              p.drops, p.runoff_m_per_yr, p.evaporation_m_per_yr,
              p.settle_fraction, p.sus_diffusion, p.swe_substeps,
              p.cfl_number, p.swe_manning_n, p.eps_wet, double(p.dt_floor_s),
              double(p.morfac), double(p.capacity_Kc_s),
              double(p.sus_settling_velocity_m_per_s),
              double(p.transverse_slope_coeff), double(p.repose_angle_deg),
              double(p.talus_relaxation_per_yr), p.cycles,
              p.cycles > 0 ? " (phase 1 on)" : " (phase 1 off)");

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

  SimStats st;
  RunSim(p, g, st, true);
  std::printf("protogen: phase 0 done\n  timings (s): drops %.1f grid %.1f "
              "settle %.1f\n",
              st.t_drops, st.t_grid, st.t_settle);

  // Output boundary: world.txt + rivers.bin, off the phase-0-FINISHED grid.
  // Same function --extract-rivers calls, so the two paths cannot diverge.
  // ALWAYS runs, even when phase 1 follows below -- a `--cycles 0` run stops
  // here, which is what keeps its output byte-shaped exactly like it always
  // was; a `--cycles > 0` run's phase-1 block OVERWRITES world.txt +
  // rivers.bin with the phase-1-finished state once it completes.
  if (!WriteWorldArtifacts(BuildInputsFromGrid(g, p), p.out)) return 1;

  // Task 8 fix: PrintRunDiagnostics's own comment says Task 8's full-run
  // validation needs a BASELINE, but until this call this function only ever
  // fired after phase 1 -- a `--cycles 0` run (exactly the baseline call)
  // never printed a depression count or drainage fraction at all. Both
  // diagnostics read only the finished `g`, which right here is the
  // phase-0-finished state, so this call is well-formed with no phase-1
  // dependency; the second call below (once phase 1 completes) reports the
  // SAME two numbers again on the phase-1-finished state, so a single
  // `--cycles > 0` run prints its own before/after without needing a
  // separate `--cycles 0` invocation to compare against.
  PrintRunDiagnostics(g, "phase-0");

  int cycles_run = 0;
  if (p.cycles > 0) {
    std::printf("protogen: phase 1 -- warm start + %d cycles "
                "(%d substeps/cycle, CFL %.2f, morfac %.0f)\n",
                p.cycles, p.swe_substeps, p.cfl_number, double(p.morfac));
    using clk = std::chrono::steady_clock;
    const auto tw0 = clk::now();
    SweWarmStart(g, p);
    const auto tw1 = clk::now();
    st.t_swe_warm_start = std::chrono::duration<double>(tw1 - tw0).count();

    // `--snapshot-every` reused as a CYCLE cadence (see Params::snapshot_every):
    // run RunSweCycles in batches of at most that many cycles, dumping a
    // phase-1 snapshot after each -- so the LAST batch always lands exactly
    // on `p.cycles`, the same "always land on the final one" guarantee
    // RunSim's own step loop gives phase 0, however `cycles % snapshot_every`
    // comes out.
    //
    // ONE `SweAuditBaseline`, threaded through EVERY batch call by pointer,
    // and `cycle_offset` set to how many cycles already ran -- NOT one fresh
    // baseline per call (fix round 1: that would audit each batch against
    // its OWN start rather than phase 1's true start, silently re-baselining
    // away any leak that stays under tolerance within a single
    // `--snapshot-every` window; see SweAuditBaseline's own comment). This
    // is what makes the sediment-mass audit a whole-phase-1 statement again
    // regardless of how the run happens to be chunked for snapshotting.
    const int cadence = std::max(1, p.snapshot_every);
    SweAuditBaseline audit;
    bool aborted = false;
    std::string abort_reason;
    while (cycles_run < p.cycles) {
      const int batch = std::min(cadence, p.cycles - cycles_run);
      const SweRunResult r =
          RunSweCycles(g, p, batch, &st, &audit, cycles_run);
      if (!r.ok) {
        // `r.aborted_cycle` is already GLOBAL (RunSweCycles adds
        // `cycle_offset` itself -- see its own comment), so no batch-local
        // arithmetic is needed here. It also names exactly how many cycles
        // are trustworthy: everything before it completed and passed its
        // own post-cycle check, the aborting cycle itself did not (or, for
        // the mass-audit/non-finite checks, completed but is the one whose
        // result is in question) -- so `cycles_run` becomes that number
        // rather than the full (possibly larger) batch size. RunSweCycles
        // has already written its own abort-<cycle>-*.f32 snapshot
        // (protogen_swe.cpp's WriteAbortSnapshot), named with the SAME
        // global cycle number this message reports.
        cycles_run = r.aborted_cycle;
        abort_reason = r.reason;
        std::fprintf(stderr,
                     "protogen: phase 1 ABORTED at cycle %d of %d requested: "
                     "%s\n",
                     r.aborted_cycle, p.cycles, r.reason.c_str());
        aborted = true;
        break;
      }
      cycles_run += batch;
      char tag[64];
      std::snprintf(tag, sizeof(tag), "%04d-cycle", cycles_run);
      DumpPhase1(g, p, tag);
      std::printf("  cycle %d/%d done, snapshot '%s'\n", cycles_run,
                  p.cycles, tag);
    }

    if (!aborted) {
      // Output boundary again, off the phase-1-FINISHED grid: surface =
      // bed + lake-only water and lake_tag = water > 0 (both already inside
      // WriteWorldArtifacts), plus the three provenance fields so
      // --extract-rivers (and anything else reading world.txt) can tell a
      // phase-1 run happened and find the right dump tag.
      if (!WriteWorldArtifacts(BuildInputsFromGrid(g, p, cycles_run), p.out))
        return 1;
      PrintRunDiagnostics(g, "phase-1");
    } else {
      // Code review finding: leaving THIS branch empty is what let an
      // aborted run leave the directory self-contradictory -- see
      // HandlePhase1Abort's own comment for the finite/non-finite split and
      // why each side does what it does. Factored out (rather than inlined
      // here) so a test can drive it directly on a tiny fixture, same as
      // RunSim/RunSweCycles.
      if (!HandlePhase1Abort(g, p, cycles_run, abort_reason)) return 1;
    }

    WritePerfReport(g, p, st, cycles_run);
    if (aborted) return 1;
  } else {
    WritePerfReport(g, p, st, 0);
  }
  return 0;
}
