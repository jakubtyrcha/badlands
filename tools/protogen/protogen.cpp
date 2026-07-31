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

  // --- lakes ---
  int lake_interval = 50;
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
  std::vector<float> vol_track, mx_track, my_track;
  std::vector<uint32_t> visits;

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

void InitTerrain(Grid& g, const Params& p) {
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

void BuildLakes(Grid& g, const Params& p, int& n_lakes, float& wet_frac,
                float& deepest_m) {
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;

  static std::vector<float> filled;
  static std::vector<int32_t> outlet_of;
  PriorityFlood(g, filled, outlet_of);

  std::fill(g.lake_id.begin(), g.lake_id.end(), -1);
  std::fill(g.water.begin(), g.water.end(), 0.f);
  g.lake_outlet.clear();

  // Separate from lake_id: a pruned component has its lake_id reset, so keying
  // the outer scan off lake_id re-floods the same component once per member --
  // quadratic, and measured at 18.8 s for one call.
  static std::vector<uint8_t> seen;
  seen.assign(g.cells, 0);

  std::vector<uint32_t> stack, members;
  std::vector<float> beds;
  int lakes = 0;
  size_t wet = 0;
  deepest_m = 0.f;

  for (size_t s0 = 0; s0 < g.cells; ++s0) {
    if (seen[s0] || filled[s0] <= g.height[s0]) continue;
    members.clear(); stack.clear();
    stack.push_back(uint32_t(s0)); seen[s0] = 1;
    const float spill = filled[s0];
    // Inflow is summed over the SHORELINE, not over the lake interior.
    //
    // Discharge is deliberately not accumulated on water cells (a lake surface
    // is not a channel), so Qm3s inside a lake decays toward zero under the EMA
    // within ~1/lrate steps. Reading inflow there makes every lake starve:
    // Q_in -> 0, so A_eq = Q_in/E -> 0 and the lake prunes itself out of
    // existence within a few hundred steps. That, not outlet incision, is why
    // lakes were vanishing. The dry cells around the rim carry the real
    // discharge, so the rivers arriving are counted there.
    float inflow_m3_s = 0.f;
    while (!stack.empty()) {
      const uint32_t c = stack.back(); stack.pop_back();
      members.push_back(c);
      const int x = int(c) % g.n, y = int(c) / g.n;
      static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nx = x + dx[k], ny = y + dy[k];
        if (nx < 0 || ny < 0 || nx >= g.n || ny >= g.n) continue;
        const size_t j = g.idx(nx, ny);
        if (filled[j] <= g.height[j]) {
          // Dry neighbour: it is shoreline. Count what it delivers if it drains
          // toward us, which for a cell on a lake's rim it does by definition.
          inflow_m3_s += g.Qm3s[j];
          continue;
        }
        if (seen[j]) continue;
        if (std::fabs(filled[j] - spill) > 1e-5f) continue;
        seen[j] = 1;
        stack.push_back(uint32_t(j));
      }
    }

    // Water balance, in real units: steady state is inflow == evaporation over
    // the surface, so the lake settles at A_eq = Q_in / E, capped by the basin
    // (once it reaches the spill level the surplus simply leaves).
    float level = spill;
    if (p.evaporation_m_per_yr > 0.f) {
      const double inflow_m3_yr = double(peak_Q_m3_s) * kSecondsPerYear;
      const size_t k_eq =
          size_t(inflow_m3_yr / double(p.evaporation_m_per_yr) / cell_area);
      if (k_eq < members.size()) {
        beds.clear();
        for (uint32_t c : members) beds.push_back(g.height[c]);
        std::sort(beds.begin(), beds.end());
        level = beds[k_eq];  // the level whose submerged area is k_eq cells
      }
    }

    size_t area_cells = 0;
    float max_depth = 0.f;
    for (uint32_t c : members) {
      const float d = level - g.height[c];
      if (d > 0.f) { ++area_cells; max_depth = std::max(max_depth, d); }
    }
    if (float(area_cells) * cell_area < p.min_lake_area_m2 ||
        max_depth * p.relief_m < p.min_lake_depth_m)
      continue;

    for (uint32_t c : members) {
      const float d = level - g.height[c];
      if (d > 0.f) { g.water[c] = d; g.lake_id[c] = lakes; ++wet; }
    }
    g.lake_outlet.push_back(outlet_of[s0] >= 0 ? outlet_of[s0] : int32_t(s0));
    deepest_m = std::max(deepest_m, max_depth * p.relief_m);
    ++lakes;
  }
  n_lakes = lakes;
  wet_frac = float(double(wet) / double(g.cells));
}

void Cascade(Grid& g, const Params& p, int x, int y, float max_diff);

// ------------------------------------------------------------------- descend

// The reference's descend, verbatim, plus a lake branch. Writes go to the
// worker's scratch rather than the grid, so workers never collide.
void Descend(Grid& g, const Params& p, float px, float py) {
  const float cell_m = p.world_m / float(p.res);
  const float scale = p.relief_m / cell_m;  // height units per cell width
  // Repose threshold in height units: tan(angle) * cell / relief.
  const float max_diff = std::tan(p.repose_angle_deg * 3.14159265f / 180.0f) *
                         cell_m / p.relief_m;
  V2 pos{px, py}, speed{0.f, 0.f};
  float volume = 1.0f, sediment = 0.0f;

  for (int age = 0; age < p.max_age; ++age) {
    const int x = int(pos.x), y = int(pos.y);
    if (g.oob(x, y)) return;
    const size_t here = g.idx(x, y);
    ++g.visits[here];

    if (volume < p.min_vol) { g.height[here] += sediment; return; }

    // --- standing water -------------------------------------------------
    if (g.water[here] > 0.f) {
      const int32_t lid = g.lake_id[here];
      if (lid < 0 || lid >= int32_t(g.lake_outlet.size())) return;
      const int32_t target = g.lake_outlet[lid];

      // Sediment DISPERSES rather than being dumped. A laden inflow is a
      // hypopycnal plume: velocity collapses at the mouth, the load settles
      // over a length L = u*h/w_s, and the steady state of
      // advection-diffusion-settling thins exponentially. So the load lost
      // crossing one cell is qs*(1 - exp(-cell/L)) -- the flux DIVERGENCE.
      // Depositing the flux itself instead diverges hard (measured 2e5 m of
      // relief) and is dimensionally the wrong quantity.
      const float depth_m = g.water[here] * p.relief_m;
      const float L_m = std::clamp(
          p.river_mouth_velocity_m_per_s * depth_m /
              std::max(p.settling_velocity_m_per_s, 1e-9f),
          cell_m, 64.0f * cell_m);
      const float f_dep = 1.0f - std::exp(-cell_m / L_m);
      // Sediment DISPLACES water: what lands on the bed comes out of the water
      // column, so the lake shallows as it fills and stops being a lake when
      // full. Without this the bed rises while `water` stays stale until the
      // next rebuild 50 steps later, and nothing bounds it -- measured, that
      // ran the terrain to 16 km.
      const float drop = std::min(sediment * f_dep, g.water[here]);
      sediment -= drop;
      g.height[here] += drop;
      g.water[here] -= drop;
      if (g.water[here] <= 0.0f) {
        g.water[here] = 0.0f;
        g.lake_id[here] = -1;  // silted up; it is land again until the rebuild
      }
      // Spread the delta instead of leaving a spike. The lake branch skipped
      // the cascade entirely, so deposits had nothing to relax them.
      Cascade(g, p, x, y, max_diff);

      // Cross to the spill point so discharge continues downstream instead of
      // being swallowed by the lake. Discharge is NOT accumulated here: a lake
      // surface is not a channel, and stamping it inflates the field.
      const float tx = float(int(target) % g.n) + 0.5f;
      const float ty = float(int(target) / g.n) + 0.5f;
      const V2 dir = unit(V2{tx - pos.x, ty - pos.y});
      if (len(dir) <= 0.f) return;
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
    if (g.oob(nx, ny)) return;
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
    for (int d = 0; d < p.drops; ++d) { sx[d] = uni(rng); sy[d] = uni(rng); }

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
    for (int d = 0; d < p.drops; ++d) Descend(g, p, sx[d], sy[d]);

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
    if (step % p.lake_interval == 0)
      BuildLakes(g, p, n_lakes, wet_frac, deepest_m);
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
      std::printf("  step %4d/%d  relief %8.1f m  lakes %4d  wet %5.2f%%\n"
                  "      visits/cell: touched %zu  mean %.2f  p50 %u  p99 %u  max %u\n",
                  step, p.steps, (hi - lo) * p.relief_m, n_lakes,
                  100.f * wet_frac, v.size(),
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
