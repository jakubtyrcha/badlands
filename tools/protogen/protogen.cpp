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

namespace {

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
      Deposit(g, here, carried_mass());
      g.deposited_death += double(carried_mass());
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
  // Reached max_age still carrying a load. The reference deposits it here;
  // dropping that made this a silent mass sink, and it is the DOMINANT exit --
  // volume only decays to 0.61 over 500 steps, so the volume < min_vol branch
  // above is unreachable in practice.
  Deposit(g, last_cell, carried_mass());
  g.deposited_death += double(carried_mass());
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
  // No standing water any more: written as zeros so downstream readers
  // (WriteWorldArtifacts, RunExtractRivers, mapview/game consumers of the
  // dump) keep seeing a water raster of the shape they expect.
  const std::vector<float> zero_water(g.cells, 0.f);
  write("water", zero_water, 1.0f);
  write("discharge", g.discharge, 1.0f);
  write("Q", g.Qm3s, 1.0f);                // m^3/s
  write("soil", g.soil, p.relief_m);       // metres of erodible cover
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
  double t_drops = 0, t_grid = 0;
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
      std::printf("  step %4d/%d  relief %8.1f m\n"
                  "      mass: sum(h) %.4e  death-dep %.4e  offmap %.4e\n"
                  "      visits/cell: touched %zu  mean %.2f  p50 %u  p99 %u  max %u\n",
                  step, p.steps, (hi - lo) * p.relief_m, hsum,
                  g.deposited_death, g.lost_offmap, v.size(),
                  v.empty() ? 0.0 : tot / double(v.size()),
                  v.empty() ? 0u : v[v.size() / 2],
                  v.empty() ? 0u : v[size_t(v.size() * 0.99)],
                  v.empty() ? 0u : v.back());
      std::fflush(stdout);
    }
  }

  st.t_drops = t_drops; st.t_grid = t_grid;
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
  // No standing water any more (see the header note); zeros keep
  // WriteWorldArtifacts/SoilCutoffs/river extraction running the 0-lake case
  // they already handle.
  in.water_m = mg::Field2D<float>(g.n, g.n);
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
              "  runoff %.2f m/yr, evaporation %.2f m/yr\n",
              p.res, p.res, p.world_m, cell_m, workers, p.relief_m, p.steps,
              p.drops, p.runoff_m_per_yr, p.evaporation_m_per_yr);

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
  std::printf("protogen: done\n  timings (s): drops %.1f grid %.1f\n",
              st.t_drops, st.t_grid);

  // Output boundary: world.txt + rivers.bin, off the FINISHED grid. Same
  // function --extract-rivers calls, so the two paths cannot diverge.
  if (!WriteWorldArtifacts(BuildInputsFromGrid(g, p), p.out)) return 1;
  return 0;
}
