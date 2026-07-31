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
  bool bowl = false;
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
  float river_mouth_velocity_m_per_s = 1.0f;  // for the lobe length L = u*h/w_s
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
  // CAP on the spread half-angle, not the spread itself -- the angle comes
  // from atan(u_turb/v). Defaulted high so it does NOT bind: at 35 deg it
  // clipped every turbulence above ~0.6 m/s to the same value, silently
  // killing the knob exactly as the old lobe-length clamp did. 0 disables
  // wander entirely (straight line), which is still the A/B control.
  float plume_wander_deg = 85.0f;
  // Ambient turbulent velocity in the lake. With the jet's centreline speed v,
  // the spread half-angle is atan(u_turb / v): narrow while the inflow's
  // momentum dominates, widening as it decays. plume_wander_deg is the CAP, so
  // --wander 0 still reproduces a straight line exactly.
  float jet_turbulence_m_per_s = 0.30f;
  // Regime width w = k_w*sqrt(Q), reused from the repo's erosion.hpp. Sets the
  // jet's inlet width, which is the length scale its decay is measured in.
  float channel_width_coeff = 5.0f;

  // --- lakes ---
  // Water is accumulated EVERY step (see UpdateLakes); this only sets how often
  // the basin topology is re-derived by priority flood.
  int lake_interval = 50;
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

// Per-worker scatter buffers: dense for O(1) accumulation with no atomics, plus
// a dirty list so the merge touches only visited cells.
struct Scratch {
  std::vector<float> dz, dvol, dmx, dmy;
  std::vector<uint8_t> marked;
  std::vector<uint32_t> dirty;
  void init(size_t cells) {
    dz.assign(cells, 0.f); dvol.assign(cells, 0.f);
    dmx.assign(cells, 0.f); dmy.assign(cells, 0.f);
    marked.assign(cells, 0);
    dirty.reserve(1u << 18);
  }
  inline void touch(size_t i) {
    if (!marked[i]) { marked[i] = 1; dirty.push_back(uint32_t(i)); }
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

void InitTerrain(Grid& g, const Params& p) {
  if (p.bowl) { InitBowl(g, p); return; }
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
void BuildLakes(Grid& g, const Params& p, std::vector<Lake>& lakes) {
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

  // Carry stored water across a rebuild: match by outlet cell, so a basin that
  // survived keeps its lake instead of restarting empty every 50 steps.
  for (Lake& lk : next) {
    bool matched = false;
    for (const Lake& old : lakes) {
      if (old.outlet == lk.outlet) {
        lk.volume_m3 = old.volume_m3;
        matched = true;
        break;
      }
    }
    if (!matched && p.prefill) {
      double v = 0.0;
      for (uint32_t c : lk.members) v += std::max(0.f, lk.spill - g.height[c]);
      lk.volume_m3 = v * double(cell_area) * double(p.relief_m);
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
    const double area_m2 = double(wet_cells) * cell_area;

    const double gain = inflow_m3_s * kSecondsPerYear * double(p.dt_years);
    const double loss = double(p.evaporation_m_per_yr) * area_m2 * double(p.dt_years);
    lk.volume_m3 = std::max(0.0, lk.volume_m3 + gain - loss);

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
  // Jet state, valid once the particle is in standing water.
  bool in_jet = false;
  V2 jet_dir{0.f, 0.f};
  float jet_x = 0.f, jet_width_m = 0.f;

  for (int age = 0; age < p.max_age; ++age) {
    const int x = int(pos.x), y = int(pos.y);
    if (g.oob(x, y)) { g.lost_offmap += double(sediment); return; }
    const size_t here = g.idx(x, y);
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
      if (lid < 0 || lid >= int32_t(g.lake_outlet.size())) return;
      const int32_t target = g.lake_outlet[lid];

      float drop;
      if (p.disperse) {
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
            p.river_mouth_velocity_m_per_s * depth_m /
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
      Cascade(g, p, x, y, max_diff);

      // Cross to the spill point so discharge continues downstream instead of
      // being swallowed. Discharge is NOT accumulated here: a lake surface is
      // not a channel, and stamping it inflates the field.
      // On entry, the particle becomes a JET: it keeps the heading it arrived
      // with rather than turning instantly toward the outlet, which is what it
      // did before and why every inflow behaved identically regardless of how
      // fast it arrived.
      if (!in_jet) {
        in_jet = true;
        jet_dir = len(speed) > 0.f ? unit(speed) : unit(V2{0.f, 1.f});
        jet_x = 0.f;
        jet_width_m = std::max(
            p.channel_width_coeff * std::sqrt(std::max(g.Qm3s[here], 1e-9f)),
            cell_m);
      }
      // Round-jet centreline decay, v = v0 / (1 + x / 6.2 D).
      const float v_jet = p.river_mouth_velocity_m_per_s /
                          (1.0f + jet_x / (6.2f * jet_width_m));
      const float tx = float(int(target) % g.n) + 0.5f;
      const float ty = float(int(target) / g.n) + 0.5f;
      const V2 toOut = unit(V2{tx - pos.x, ty - pos.y});
      if (len(toOut) <= 0.f) return;

      // Momentum holds the heading while the jet is fast; the outlet takes over
      // as it decays.
      const float pull = std::clamp(
          1.0f - v_jet / std::max(p.river_mouth_velocity_m_per_s, 1e-6f),
          0.0f, 1.0f);
      V2 dir = unit(V2{(1.0f - pull) * jet_dir.x + pull * toOut.x,
                       (1.0f - pull) * jet_dir.y + pull * toOut.y});
      if (len(dir) <= 0.f) dir = toOut;

      if (p.plume_wander_deg > 0.f) {
        // Spread half-angle from the momentum-to-turbulence ratio, capped by
        // --wander so the knob still bounds it.
        const float sigma = std::min(
            std::atan(p.jet_turbulence_m_per_s / std::max(v_jet, 1e-3f)),
            p.plume_wander_deg * 3.14159265f / 180.f);
        std::normal_distribution<float> jitter(0.f, sigma);
        const float a = std::atan2(dir.y, dir.x) + jitter(rng);
        dir = V2{std::cos(a), std::sin(a)};
      }
      jet_dir = dir;
      jet_x += cell_m;
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

    float c_eq = (1.0f + p.entrainment * g.discharge[here]) *
                 (g.height[here] - g.height[there]);
    if (c_eq < 0.f) c_eq = 0.f;
    const float cdiff = c_eq - sediment;
    sediment += p.deposition_rate * cdiff;
    g.height[here] -= volume * p.deposition_rate * cdiff;

    Cascade(g, p, nx, ny, max_diff);

    sediment /= (1.0f - p.evap_rate);
    volume *= (1.0f - p.evap_rate);
  }
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
    const float excess = nb[k].d - max_diff;
    if (excess <= 0.f) break;  // sorted: nothing after this exceeds either
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
    if (!fp) return;
    std::fwrite(tmp.data(), sizeof(float), tmp.size(), fp);
    std::fclose(fp);
  };
  write("height", g.height, p.relief_m);   // metres
  write("water", g.water, p.relief_m);     // metres
  write("discharge", g.discharge, 1.0f);
  write("Q", g.Qm3s, 1.0f);                // m^3/s
}

}  // namespace

int main(int argc, char** argv) {
  Params p;
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
    else if (a == "--settling") p.settling_velocity_m_per_s = std::stof(nxt());
    else if (a == "--mouth-velocity") p.river_mouth_velocity_m_per_s = std::stof(nxt());
    else if (a == "--lake-interval") p.lake_interval = std::stoi(nxt());
    else if (a == "--min-lake-area") p.min_lake_area_m2 = std::stof(nxt());
    else if (a == "--min-lake-depth") p.min_lake_depth_m = std::stof(nxt());
    else if (a == "--entrainment") p.entrainment = std::stof(nxt());
    else if (a == "--deposition") p.deposition_rate = std::stof(nxt());
    else if (a == "--lrate") p.lrate = std::stof(nxt());
    else if (a == "--repose") p.repose_angle_deg = std::stof(nxt());
    else if (a == "--settling") p.settling = std::stof(nxt());
    else if (a == "--snapshot-every") p.snapshot_every = std::stoi(nxt());
    else if (a == "--bowl") p.bowl = true;
    else if (a == "--no-disperse") p.disperse = false;
    else if (a == "--prefill") p.prefill = true;
    else if (a == "--wander") p.plume_wander_deg = std::stof(nxt());
    else if (a == "--turbulence") p.jet_turbulence_m_per_s = std::stof(nxt());
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

  const float cell_m = p.world_m / float(p.res);
  // Each particle stands for the runoff over its share of the map, which is how
  // the dimensionless volume track converts to a real discharge.
  const double q_per_unit_vol_m3_s =
      double(p.runoff_m_per_yr) * double(p.world_m) * double(p.world_m) /
      double(p.drops) / kSecondsPerYear;

  Grid g(p.res);
  InitTerrain(g, p);

  const unsigned workers = std::max(1u, badlands::GetWorkerThreadCount());
  std::vector<Scratch> scratch(workers);
  for (auto& s : scratch) s.init(g.cells);

  std::printf("protogen: %dx%d cells, %.0f m world, %.1f m cells, %d workers\n"
              "  relief %.0f m, %d steps x %d drops\n"
              "  runoff %.2f m/yr, evaporation %.2f m/yr, w_s %.1e m/s\n",
              p.res, p.res, p.world_m, cell_m, workers, p.relief_m, p.steps,
              p.drops, p.runoff_m_per_yr, p.evaporation_m_per_yr,
              p.settling_velocity_m_per_s);

  Dump(g, p, "0000-initial");
  std::mt19937 rng(p.seed ^ 0x9e3779b9u);
  std::uniform_real_distribution<float> uni(1.0f, float(p.res - 2));

  std::vector<Lake> lakes;
  int n_lakes = 0; float wet_frac = 0.f, deepest_m = 0.f;
  using clk = std::chrono::steady_clock;
  double t_drops = 0, t_merge = 0, t_grid = 0, t_lake = 0;
  auto secs = [](clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
  };

  std::vector<float> sx(p.drops), sy(p.drops);
  for (int step = 1; step <= p.steps; ++step) {
    // Spawn points drawn on the main thread, so the particle set is a pure
    // function of the seed and independent of how work is chunked.
    if (p.bowl) {
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

    auto tB = clk::now(); t_drops += secs(tA, tB);
    auto tC = tB;
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
    if (step % p.lake_interval == 0 || step == 1) BuildLakes(g, p, lakes);
    UpdateLakes(g, p, lakes, n_lakes, wet_frac, deepest_m);
    t_lake += secs(tD, clk::now());

    if (step % p.snapshot_every == 0 || step == p.steps) {
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
  std::printf("protogen: done\n  timings (s): drops %.1f merge %.1f grid %.1f "
              "lakes %.1f\n", t_drops, t_merge, t_grid, t_lake);
  return 0;
}
