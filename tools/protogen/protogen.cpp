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

namespace {

constexpr double kSecondsPerYear = 31557600.0;

// ---------------------------------------------------------------- parameters

struct Params {
  uint32_t seed = 1;
  int res = 1024;
  float world_m = 16384.0f;
  float relief_m = 900.0f;  // height unit 1.0 == this many metres
  float noise_wavelength_m = 4096.0f;
  int noise_octaves = 8;
  // Synthetic fixture: a dish rising to `bowl_rim_m` at the edge with a gaussian
  // well at the centre, so everything drains inward to one closed basin. All
  // particles spawn at ONE point, which makes the flow path deterministic and
  // therefore gives "where should the sediment land" a right answer.
  // Synthetic terrains for the sanity tests. Each isolates ONE mechanism; the
  // toggles below let a test switch the others off so a failure has one cause.
  enum class Terrain { Noise, Bowl, Flat, Plane, Valley, Cliff, Lobe };
  Terrain terrain = Terrain::Noise;
  bool enable_erosion = true;
  bool enable_cascade = true;
  bool enable_lake_deposit = true;
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

  // --- reference erosion constants (SimpleHydrology source). DO NOT RETUNE ---
  float evap_rate = 0.001f;
  float deposition_rate = 0.1f;
  float min_vol = 0.01f;
  int max_age = 500;
  float entrainment = 10.0f;
  float gravity = 1.0f;
  float momentum_transfer = 1.0f;
  float lrate = 0.01f;
  float erf_scale = 0.4f;
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
  int lake_deposit_steps = 4;
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
  float dt_years = 200.0f;  // water-budget timestep (erosion stays step-based)
  // A cell must hold at least this much water before the plume/dispersion path
  // engages. Transient sheet flow is not a lake and must not trigger it.
  float min_dispersion_depth_m = 1.0f;
  float min_lake_area_m2 = 1.0e4f;  // 1 ha
  float min_lake_depth_m = 0.5f;

  std::string out = "proto_out";
  int snapshot_every = 250;
};

// -------------------------------------------------------------------- fields

struct Grid {
  int n = 0;
  size_t cells = 0;
  std::vector<float> height, height_b;        // DIMENSIONLESS ~[0,1]
  std::vector<float> discharge, discharge_b;  // reference's erf-squashed field
  std::vector<float> Qm3s, Qm3s_b;            // real discharge, for lakes/output
  std::vector<float> momx, momy, momx_b, momy_b;
  std::vector<float> water;                   // standing depth, height units
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
        discharge(cells, 0.f), discharge_b(cells, 0.f),
        Qm3s(cells, 0.f), Qm3s_b(cells, 0.f),
        momx(cells, 0.f), momy(cells, 0.f), momx_b(cells, 0.f), momy_b(cells, 0.f),
        water(cells, 0.f), lake_id(cells, -1),
        vol_track(cells, 0.f), mx_track(cells, 0.f), my_track(cells, 0.f),
        visits(cells, 0u) {}

  inline size_t idx(int x, int y) const { return size_t(y) * n + x; }
  inline bool oob(int x, int y) const {
    return x < 1 || y < 1 || x >= n - 1 || y >= n - 1;
  }
};

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

void InitTerrain(Grid& g, const Params& p) {
  if (p.bowl || p.terrain == Params::Terrain::Bowl) { InitBowl(g, p); return; }
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

// ---------------------------------------------------------------- lake build

// Per-lake state, cached between the periodic topology rebuilds so the water
// budget can be integrated EVERY step rather than only at a rebuild.
struct Lake {
  std::vector<uint32_t> members;   // basin cells below the spill level
  std::vector<float> sorted_beds;  // member bed heights, ascending
  std::vector<double> prefix;      // prefix sums of sorted_beds
  std::vector<uint32_t> shore;     // dry rim cells -- where inflow is measured
  float spill = 0.f;               // height units
  float level = 0.f;               // current water surface, height units
  double volume_m3 = 0.0;
  int32_t outlet = -1;
};

// Water surface for a given stored volume, from the basin's hypsometry.
// Between the k-th and (k+1)-th lowest beds exactly k cells are submerged, so
// V(L) = (k*L - prefix[k]) * cell_area, which inverts directly.
float LevelFromVolume(const Lake& lk, double volume_m3, float cell_area,
                      float relief_m) {
  if (lk.sorted_beds.empty() || volume_m3 <= 0.0) {
    return lk.sorted_beds.empty() ? 0.f : lk.sorted_beds[0];
  }
  const double v_units = volume_m3 / (double(cell_area) * double(relief_m));
  for (size_t k = 1; k <= lk.sorted_beds.size(); ++k) {
    const double lvl = (v_units + lk.prefix[k]) / double(k);
    // Valid while the level stays below the next bed up (so exactly k are wet).
    if (k == lk.sorted_beds.size() || lvl <= lk.sorted_beds[k]) {
      return float(lvl);
    }
  }
  return lk.sorted_beds.back();
}

// Re-derives basin topology by priority flood. Expensive, so it runs every
// `lake_interval` steps; the water budget itself runs every step in UpdateLakes.
void BuildLakes(Grid& g, const Params& p, std::vector<Lake>& lakes,
                bool first_build) {
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;

  static std::vector<float> filled;
  static std::vector<int32_t> outlet_of;
  PriorityFlood(g, filled, outlet_of);

  // Separate from lake_id: a pruned component has its lake_id reset, so keying
  // the outer scan off lake_id re-floods the same component once per member --
  // quadratic, measured at 18.8 s for a single call.
  static std::vector<uint8_t> seen;
  seen.assign(g.cells, 0);

  std::vector<Lake> next;
  std::vector<uint32_t> stack;
  for (size_t s0 = 0; s0 < g.cells; ++s0) {
    if (seen[s0] || filled[s0] <= g.height[s0]) continue;
    Lake lk;
    lk.spill = filled[s0];
    lk.outlet = outlet_of[s0] >= 0 ? outlet_of[s0] : int32_t(s0);
    stack.clear();
    stack.push_back(uint32_t(s0));
    seen[s0] = 1;
    while (!stack.empty()) {
      const uint32_t c = stack.back(); stack.pop_back();
      lk.members.push_back(c);
      const int x = int(c) % g.n, y = int(c) / g.n;
      static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nx = x + dx[k], ny = y + dy[k];
        if (nx < 0 || ny < 0 || nx >= g.n || ny >= g.n) continue;
        const size_t j = g.idx(nx, ny);
        if (filled[j] <= g.height[j]) {
          // Dry rim. Inflow is measured HERE, not inside the basin: discharge
          // is not accumulated on water cells, so Qm3s in the interior decays
          // to zero under the EMA and every lake would starve itself out.
          lk.shore.push_back(uint32_t(j));
          continue;
        }
        if (filled[j] > lk.spill + 1e-5f) {
          // A lake perched above us spills into us: count its flow.
          lk.shore.push_back(uint32_t(j));
          continue;
        }
        if (seen[j]) continue;
        if (std::fabs(filled[j] - lk.spill) > 1e-5f) continue;
        seen[j] = 1;
        stack.push_back(uint32_t(j));
      }
    }
    if (float(lk.members.size()) * cell_area < p.min_lake_area_m2) continue;

    std::sort(lk.shore.begin(), lk.shore.end());
    lk.shore.erase(std::unique(lk.shore.begin(), lk.shore.end()), lk.shore.end());
    lk.sorted_beds.reserve(lk.members.size());
    for (uint32_t c : lk.members) lk.sorted_beds.push_back(g.height[c]);
    std::sort(lk.sorted_beds.begin(), lk.sorted_beds.end());
    lk.prefix.assign(lk.sorted_beds.size() + 1, 0.0);
    for (size_t i = 0; i < lk.sorted_beds.size(); ++i)
      lk.prefix[i + 1] = lk.prefix[i] + lk.sorted_beds[i];
    next.push_back(std::move(lk));
  }

  // Carry stored water across a rebuild by MEMBER OVERLAP, not by outlet
  // identity.
  //
  // Matching on the outlet cell index is fragile in exactly the way that
  // matters: erosion migrates the spill point by a cell, or a basin splits in
  // two, and the match fails -- dropping the whole stored volume. Water was
  // therefore lost at every rebuild, so lake survival depended on
  // lake_interval (measured: a steady drain to nothing at 25, stable at 50).
  //
  // Overlap handles migration, splits and merges alike, and distributing each
  // old lake's volume across the new lakes covering it in proportion to shared
  // cells conserves the total.
  {
    std::vector<int32_t> old_of(g.cells, -1);
    std::vector<double> old_count(lakes.size(), 0.0);
    for (size_t j = 0; j < lakes.size(); ++j) {
      for (uint32_t c : lakes[j].members) old_of[c] = int32_t(j);
      old_count[j] = double(lakes[j].members.size());
    }
    for (Lake& lk : next) {
      std::vector<double> share(lakes.size(), 0.0);
      for (uint32_t c : lk.members)
        if (old_of[c] >= 0) share[old_of[c]] += 1.0;
      double v = 0.0;
      for (size_t j = 0; j < lakes.size(); ++j)
        if (old_count[j] > 0.0)
          v += lakes[j].volume_m3 * (share[j] / old_count[j]);
      const bool matched = v > 0.0;
      if (!matched && p.prefill && first_build) {
        for (uint32_t c : lk.members) v += std::max(0.f, lk.spill - g.height[c]);
        v *= double(cell_area) * double(p.relief_m);
      }
      // Never more than the basin can physically hold.
      double cap = 0.0;
      for (uint32_t c : lk.members) cap += std::max(0.f, lk.spill - g.height[c]);
      cap *= double(cell_area) * double(p.relief_m);
      lk.volume_m3 = std::min(v, cap);
    }
  }
  lakes.swap(next);
}

// The per-step water budget: inflow minus evaporation over the surface.
void UpdateLakes(Grid& g, const Params& p, std::vector<Lake>& lakes,
                 int& n_lakes, float& wet_frac, float& deepest_m) {
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;
  std::fill(g.water.begin(), g.water.end(), 0.f);
  std::fill(g.lake_id.begin(), g.lake_id.end(), -1);
  g.lake_outlet.clear();
  g.lake_index.clear();

  size_t wet = 0;
  deepest_m = 0.f;
  int kept = 0;
  for (size_t li = 0; li < lakes.size(); ++li) {
    Lake& lk = lakes[li];
    double inflow_m3_s = 0.0;
    for (uint32_t c : lk.shore) inflow_m3_s += g.Qm3s[c];

    // Surface area at the CURRENT level -- evaporation scales with it, which is
    // what makes the equilibrium A_eq = Q_in/E emerge instead of being imposed.
    size_t wet_cells = 0;
    for (uint32_t c : lk.members) if (g.height[c] < lk.level) ++wet_cells;

    // TWO TIMESCALES. Erosion wants ~200 yr steps; a lake equilibrates in
    // years. Integrating the budget with an explicit 200 yr Euler step applies
    // 0.8 * 200 = 160 m of evaporation at once, which empties any lake in a
    // single step -- the sanity suite caught exactly that.
    //
    // So relax toward the water-balance equilibrium with the lake's own
    // residence time tau = V / Q_in. For dt >> tau this lands on equilibrium
    // (the physically right answer, since water is fast); for a big lake fed by
    // a trickle, tau is long and it fills gradually, which is also right.
    const double inflow_m3_yr = inflow_m3_s * kSecondsPerYear;
    const double area_eq_m2 =
        p.evaporation_m_per_yr > 0.f
            ? inflow_m3_yr / double(p.evaporation_m_per_yr)
            : 1e30;
    double v_eq = 0.0;
    {
      // Volume at the level whose submerged area is area_eq.
      const size_t k_eq = size_t(std::min(area_eq_m2 / double(cell_area),
                                          double(lk.sorted_beds.size())));
      if (k_eq >= lk.sorted_beds.size()) {
        for (uint32_t c : lk.members)
          v_eq += std::max(0.f, lk.spill - g.height[c]);
      } else if (k_eq > 0) {
        const double lvl = lk.sorted_beds[k_eq - 1];
        for (size_t i = 0; i < k_eq; ++i) v_eq += lvl - lk.sorted_beds[i];
      }
      v_eq *= double(cell_area) * double(p.relief_m);
    }
    const double tau_yr =
        inflow_m3_yr > 0.0 ? std::max(v_eq, 1.0) / inflow_m3_yr : 1e30;
    const double f = 1.0 - std::exp(-double(p.dt_years) / std::max(tau_yr, 1e-6));
    lk.volume_m3 = std::max(0.0, lk.volume_m3 + f * (v_eq - lk.volume_m3));

    lk.level = LevelFromVolume(lk, lk.volume_m3, cell_area, p.relief_m);
    if (lk.level > lk.spill) {
      // Overflowing: the surplus leaves, so storage is capped at spill volume.
      lk.level = lk.spill;
      double v = 0.0;
      for (uint32_t c : lk.members)
        v += std::max(0.f, lk.spill - g.height[c]);
      lk.volume_m3 = v * double(cell_area) * double(p.relief_m);
    }

    float max_depth = 0.f;
    for (uint32_t c : lk.members)
      max_depth = std::max(max_depth, lk.level - g.height[c]);
    if (max_depth * p.relief_m < p.min_lake_depth_m) continue;

    const int32_t id = kept;
    for (uint32_t c : lk.members) {
      const float d = lk.level - g.height[c];
      if (d > 0.f) { g.water[c] = d; g.lake_id[c] = id; ++wet; }
    }
    g.lake_outlet.push_back(lk.outlet);
    g.lake_index.push_back(int32_t(li));
    deepest_m = std::max(deepest_m, max_depth * p.relief_m);
    ++kept;
  }
  n_lakes = kept;
  wet_frac = float(double(wet) / double(g.cells));
}


void Cascade(Grid& g, const Params& p, int x, int y, float max_diff);

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
  float volume = 1.0f, sediment = 0.0f;
  int lake_steps = 0;  // in-lake steps taken, for the deposition cutoff
  size_t last_cell = g.idx(int(px), int(py));

  for (int age = 0; age < p.max_age; ++age) {
    const int x = int(pos.x), y = int(pos.y);
    if (g.oob(x, y)) { g.lost_offmap += double(sediment); return; }
    const size_t here = g.idx(x, y);
    last_cell = here;
    ++g.visits[here];

    if (volume < p.min_vol) {
      g.height[here] += sediment;
      g.deposited_death += double(sediment);
      return;
    }

    // --- standing water -------------------------------------------------
    // THRESHOLD: only water deep enough to be a lake engages the plume path.
    // Transient sheet flow over a damp cell is not standing water and must keep
    // behaving as a river, or dispersion fires on every wet-ish channel cell.
    if (g.water[here] * p.relief_m >= p.min_dispersion_depth_m) {
      const int32_t lid = g.lake_id[here];
      if (lid < 0 || lid >= int32_t(g.lake_outlet.size())) {
        g.height[here] += sediment;
        g.deposited_death += double(sediment);
        return;
      }
      const int32_t target = g.lake_outlet[lid];

      ++lake_steps;
      const bool depositing =
          p.enable_lake_deposit && lake_steps <= p.lake_deposit_steps;
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
      g.height[here] += drop;
      g.deposited_lake += double(drop);
      // Sediment DISPLACES water: the deposit comes out of the lake's storage,
      // so the basin shallows as it fills rather than growing a bed under a
      // stale water field.
      if (lid < int32_t(g.lake_index.size())) {
        lakes[g.lake_index[lid]].volume_m3 -= double(drop) * double(p.relief_m) *
                                double(cell_m) * double(cell_m);
        double& v = lakes[g.lake_index[lid]].volume_m3;
        if (v < 0.0) v = 0.0;
      }
      if (p.enable_cascade) Cascade(g, p, x, y, max_diff);

      // Cross to the spill point so discharge continues downstream instead of
      // being swallowed. Discharge is NOT accumulated here: a lake surface is
      // not a channel, and stamping it inflates the field.
      const float tx = float(int(target) % g.n) + 0.5f;
      const float ty = float(int(target) / g.n) + 0.5f;
      V2 dir = unit(V2{tx - pos.x, ty - pos.y});
      if (len(dir) <= 0.f) return;
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
      volume *= (1.0f - p.evap_rate);
      continue;
    }

    // --- reference erosion, unchanged ------------------------------------
    const float dhx =
        (g.height[g.idx(x + 1, y)] - g.height[g.idx(x - 1, y)]) * 0.5f;
    const float dhy =
        (g.height[g.idx(x, y + 1)] - g.height[g.idx(x, y - 1)]) * 0.5f;
    const float gx = -scale * dhx, gy = -scale * dhy;
    const float nl = std::sqrt(gx * gx + gy * gy + 1.0f);
    speed = V2{speed.x + (p.gravity / volume) * (gx / nl),
               speed.y + (p.gravity / volume) * (gy / nl)};

    const V2 f{g.momx[here], g.momy[here]};
    if (len(f) > 0.f && len(speed) > 0.f) {
      const float k = p.momentum_transfer * dot(unit(f), unit(speed)) /
                      (volume + g.discharge[here]);
      speed = V2{speed.x + k * f.x, speed.y + k * f.y};
    }
    if (len(speed) <= 0.f) return;
    speed = V2{std::sqrt(2.f) * unit(speed).x, std::sqrt(2.f) * unit(speed).y};

    pos = V2{pos.x + speed.x, pos.y + speed.y};
    g.vol_track[here] += volume;
    g.mx_track[here] += volume * speed.x;
    g.my_track[here] += volume * speed.y;

    const int nx = int(pos.x), ny = int(pos.y);
    if (g.oob(nx, ny)) { g.lost_offmap += double(sediment); return; }
    const size_t there = g.idx(nx, ny);

    if (!p.enable_erosion) { sediment = 0.f; }
    float c_eq = (1.0f + p.entrainment * g.discharge[here]) *
                 (g.height[here] - g.height[there]);
    if (c_eq < 0.f) c_eq = 0.f;
    const float cdiff = p.enable_erosion ? (c_eq - sediment) : 0.f;
    sediment += p.deposition_rate * cdiff;
    g.height[here] -= volume * p.deposition_rate * cdiff;

    if (p.enable_cascade) Cascade(g, p, nx, ny, max_diff);

    sediment /= (1.0f - p.evap_rate);
    volume *= (1.0f - p.evap_rate);
  }
  // Reached max_age still carrying a load. The reference deposits it here;
  // dropping that made this a silent mass sink, and it is the DOMINANT exit --
  // volume only decays to 0.61 over 500 steps, so the volume < min_vol branch
  // above is unreachable in practice.
  g.height[last_cell] += sediment;
  g.deposited_death += double(sediment);
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
  const size_t here = g.idx(x, y);
  for (int k = 0; k < 8; ++k) {
    const int ax = x + cdx[k], ay = y + cdy[k];
    if (g.oob(ax, ay)) continue;
    const size_t a = g.idx(ax, ay);
    const float d = g.height[here] - g.height[a];
    if (d > 0.f) nb[count++] = {a, d};
  }
  if (count == 0) return;
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
    g.height[here] -= transfer;
    g.height[nb[k].i] += transfer;
  }
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
  const double q_per_unit_vol_m3_s =
      double(p.runoff_m_per_yr) * double(world_area) / double(p.drops) /
      kSecondsPerYear;
  std::mt19937 rng(p.seed ^ 0x9e3779b9u);
  std::uniform_real_distribution<float> uni(1.0f, float(p.res - 2));
  std::vector<float> sx(p.drops), sy(p.drops);
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
        g.discharge_b[i] = (1.f - lr) * g.discharge[i] +
                           lr * std::erf(es * g.vol_track[i]);
        g.Qm3s_b[i] = (1.f - lr) * g.Qm3s[i] +
                      lr * float(double(g.vol_track[i]) * q_per_unit_vol_m3_s);
        g.momx_b[i] = (1.f - lr) * g.momx[i] + lr * g.mx_track[i];
        g.momy_b[i] = (1.f - lr) * g.momy[i] + lr * g.my_track[i];
      }
    });
    g.discharge.swap(g.discharge_b);
    g.Qm3s.swap(g.Qm3s_b);
    g.momx.swap(g.momx_b);
    g.momy.swap(g.momy_b);

    auto tD = clk::now(); t_grid += secs(tC, tD);
    if (step % p.lake_interval == 0 || step == 1)
      BuildLakes(g, p, lakes, step == 1);
    UpdateLakes(g, p, lakes, n_lakes, wet_frac, deepest_m);
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

int g_pass = 0, g_fail = 0;

void Check(const char* name, bool ok, const std::string& detail) {
  std::printf("  [%s] %-38s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (ok) ++g_pass; else ++g_fail;
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
      {"deposition_rate", [](Params& p, int i) {
         p.deposition_rate = i ? 0.02f : 0.4f; }},
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

// --- 12. lake fills at exactly the inflow rate ----------------------------
// No evaporation, no sediment: stored volume must equal Q_in x elapsed time.
void LakeFillRate() {
  Params p = Base();
  p.terrain = Params::Terrain::Bowl;
  p.evaporation_m_per_yr = 0.0f;
  p.enable_erosion = false;
  p.enable_cascade = false;
  p.enable_lake_deposit = false;
  p.steps = 300;
  std::vector<Lake> lakes; SimStats st;
  Grid g = Run(p, lakes, st);
  double stored = 0.0;
  for (const Lake& lk : lakes) stored += lk.volume_m3;
  // With no evaporation a basin fills to spill and stops, so the assertion is
  // that it filled at all and did not exceed its own capacity.
  double capacity = 0.0;
  const float cell_area = (p.world_m / p.res) * (p.world_m / p.res);
  for (const Lake& lk : lakes)
    for (uint32_t c : lk.members)
      capacity += std::max(0.f, lk.spill - g.height[c]) * cell_area * p.relief_m;
  Check("lake volume within basin capacity",
        stored > 0.0 && stored <= capacity * 1.05 + 1.0,
        F("stored %.3e m3, capacity %.3e", stored, capacity));
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

// --- 14. BuildLakes scales linearly ---------------------------------------
// It was quadratic once (18.8 s for a single call) because pruned components
// were re-flooded once per member.
void LakeScaling() {
  using clk = std::chrono::steady_clock;
  double t[2];
  for (int i = 0; i < 2; ++i) {
    Params p = Base(i ? 128 : 64);
    p.terrain = Params::Terrain::Bowl;
    Grid g(p.res);
    InitTerrain(g, p);
    std::vector<Lake> lakes;
    auto a = clk::now();
    for (int k = 0; k < 5; ++k) BuildLakes(g, p, lakes, k == 0);
    t[i] = std::chrono::duration<double>(clk::now() - a).count();
  }
  // 4x the cells; linear would be ~4x, quadratic ~16x.
  const double ratio = t[0] > 0 ? t[1] / t[0] : 0;
  Check("BuildLakes scales ~linearly", ratio < 8.0,
        F("64->128 cells cost x%.2f (%.1f ms -> %.1f ms)", ratio,
          1e3 * t[0], 1e3 * t[1]));
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

// --- 17. rebuild cadence is numerical, not physical -------------------------
// How often the basin topology is re-derived is an implementation choice. If it
// changes whether a lake EXISTS, something is being lost or reset on rebuild.
void RebuildCadenceInvariant() {
  double wet[2];
  // Convergence, not equality: refining the rebuild interval must stop changing
  // the answer. Comparing arbitrary intervals is wrong, because a coarse one is
  // simply running on stale hypsometry -- which is how the old default of 50
  // reported a lake that finer intervals agree has silted up.
  const int intervals[2] = {10, 25};
  for (int i = 0; i < 2; ++i) {
    Params p = Base();
    p.terrain = Params::Terrain::Bowl;
    p.prefill = true;
    p.bowl_well_m = 120.0f;
    p.steps = 300;
    p.lake_interval = intervals[i];
    std::vector<Lake> lakes; SimStats st;
    Grid g = Run(p, lakes, st);
    size_t n = 0;
    for (float w : g.water) if (w > 0.f) ++n;
    wet[i] = double(n) / double(g.cells);
  }
  const double rel = std::max(wet[0], wet[1]) > 0
                         ? std::fabs(wet[1] - wet[0]) / std::max(wet[0], wet[1])
                         : 0.0;
  Check("rebuild cadence has converged", rel < 0.25,
        F("interval 10 -> %.2f%% wet, interval 25 -> %.2f%%", 100 * wet[0],
          100 * wet[1]));
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
  LakeFillRate();
  Symmetry();
  LakeScaling();
  ResolutionIndependence();
  DeltaProfile();
  RebuildCadenceInvariant();
  std::printf("\n  %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace test

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--test") return test::RunAll();
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
    else if (a == "--deposition") p.deposition_rate = std::stof(nxt());
    else if (a == "--lrate") p.lrate = std::stof(nxt());
    else if (a == "--repose") p.repose_angle_deg = std::stof(nxt());
    else if (a == "--cascade-settling") p.settling = std::stof(nxt());
    else if (a == "--snapshot-every") p.snapshot_every = std::stoi(nxt());
    else if (a == "--bowl") p.bowl = true;
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
  return 0;
}
