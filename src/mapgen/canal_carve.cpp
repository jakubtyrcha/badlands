#include "mapgen/canal_carve.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/hydrology.hpp"

namespace badlands::mapgen {

namespace {

// Compass directions in CIRCULAR order from +x, so index arithmetic mod 8 is
// angular arithmetic. hydrology.cpp's dx8/dy8 are grouped orthogonals-then-
// diagonals and are NOT circular, so they cannot be reused here.
constexpr const int* kDx = kCanalDx;
constexpr const int* kDy = kCanalDy;
constexpr float kQuarterPi = 0.78539816339f;

// Shortest turn between two compass indices, in 45-degree steps (0..4).
int turn_steps(int from, int to) {
  const int d = std::abs(from - to) % 8;
  return std::min(d, 8 - d);
}

uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// Per-agent stream keyed on (map seed, seed cell), so an agent's wander does
// not depend on how many agents ran before it.
struct Rng {
  uint32_t s;
  Rng(uint32_t a, uint32_t b) : s(hash_u32(a * 0x9e3779b9U + b) | 1u) {}
  uint32_t next() { s = hash_u32(s); return s; }
  float unit() { return static_cast<float>(next() & 0xffffffU) / 16777216.0f; }
};

}  // namespace

CanalResult carve_canals(Field2D<float>& B, const Field2D<uint8_t>& lake_mask,
                         const Field2D<float>& dist_to_plains,
                         const ErosionParams& p, float texel_m, uint32_t seed) {
  CanalResult out;
  const int w = B.width, ht = B.height;
  if (w <= 0 || ht <= 0) return out;
  out.trail_source = Field2D<int32_t>(w, ht, -1);
  out.trail_dir = Field2D<uint8_t>(w, ht, kCanalNoDir);
  if (p.canal_seed_area_m2 <= 0.0f || p.canal_depth_m <= 0.0f) return out;

  // --- seeding: highland edges carrying enough flow ---
  const FlowRouting r = route_flow(B, texel_m, kEpsilonM);
  const Field2D<float> area = accumulate_drainage(r, texel_m * texel_m);

  std::vector<int> seeds;
  for (int y = 1; y < ht - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      if (dist_to_plains.data[i] != 0.0f) continue;  // not plains
      if (lake_mask.data[i]) continue;
      if (area.data[i] < p.canal_seed_area_m2) continue;
      bool touches_highland = false;
      for (int d = 0; d < 8 && !touches_highland; ++d)
        if (dist_to_plains.at(x + kDx[d], y + kDy[d]) > 0.0f) touches_highland = true;
      if (touches_highland) seeds.push_back(static_cast<int>(i));
    }
  }
  // Descending drainage area, ties by linear index. Agents interact through
  // trails, so the order has to be defined.
  std::sort(seeds.begin(), seeds.end(), [&](int a, int b) {
    const float aa = area.data[static_cast<size_t>(a)];
    const float ab = area.data[static_cast<size_t>(b)];
    if (aa != ab) return aa > ab;
    return a < b;
  });

  // Pre-canal terrain. Banks are measured against this, so one canal's trench
  // cannot become the next one's idea of ground level.
  const Field2D<float> B0 = B;
  const int max_turn = std::clamp(
      static_cast<int>(std::lround(p.canal_max_turn_angle_rad / kQuarterPi)), 1, 3);
  const float sense = p.canal_sense_distance_texels;
  std::vector<int> survivors;

  for (const int seed_cell : seeds) {
    if (out.trail_source.data[static_cast<size_t>(seed_cell)] >= 0) continue;
    const int32_t agent = out.stats.agents++;
    Rng rng(seed, static_cast<uint32_t>(seed_cell));

    int dir = 0;  // start heading downstream, from the routing already computed
    const int32_t rcv = r.receiver[static_cast<size_t>(seed_cell)];
    if (rcv >= 0) {
      const int dx = rcv % w - seed_cell % w, dy = rcv / w - seed_cell / w;
      for (int d = 0; d < 8; ++d)
        if (kDx[d] == dx && kDy[d] == dy) dir = d;
    }

    int cell = seed_cell;
    out.trail_source.data[static_cast<size_t>(cell)] = agent;
    CanalEnd end = CanalEnd::StepCap;
    int steps = 0;

    for (; steps < p.canal_max_steps; ++steps) {
      const int cx = cell % w, cy = cell / w;

      // Speed grows slowly with distance from the source, and speed is what
      // resists turning — so a reach far from its head sweeps wider arcs than
      // a headwater does, with no discharge model needed in this pass.
      const float w_turn_eff =
          p.canal_w_turn * (1.0f + p.canal_speed_gain * static_cast<float>(steps));

      int chosen = -1, offmap_dir = -1;
      float best = 0.0f;
      survivors.clear();
      for (int t = -max_turn; t <= max_turn; ++t) {
        const int d = (dir + t + 8) % 8;
        const int nx = cx + kDx[d], ny = cy + kDy[d];
        if (nx < 0 || ny < 0 || nx >= w || ny >= ht) {
          if (offmap_dir < 0) offmap_dir = d;  // leaving the map is a real option
          continue;
        }
        const size_t j = static_cast<size_t>(ny) * w + nx;
        survivors.push_back(d);

        // Every term in METRES, so the weights read as exchange rates rather
        // than opaque gains.
        const float drop = B.data[static_cast<size_t>(cell)] - B.data[j];
        // What this step would cost to dig against the bank-relative target.
        const float excavate =
            std::max(0.0f, B.data[j] - (B0.data[j] - p.canal_depth_m));

        // Water ahead attracts, a lower one more than a higher one. There is
        // deliberately NO same-source repulsion: an agent curling back into
        // its own trail closes a meander and leaves an island, which is wanted
        // here rather than a braid to prevent.
        float water_pull = 0.0f;
        const int sx = cx + static_cast<int>(std::lround(kDx[d] * sense));
        const int sy = cy + static_cast<int>(std::lround(kDy[d] * sense));
        if (sx >= 0 && sy >= 0 && sx < w && sy < ht) {
          const size_t si = static_cast<size_t>(sy) * w + sx;
          if (out.trail_source.data[si] >= 0 || lake_mask.data[si]) {
            const float wd = B.data[static_cast<size_t>(cell)] - B.data[si];
            water_pull = p.canal_w_water_m *
                         std::clamp(wd / p.canal_water_falloff_m, -1.0f, 1.0f);
          }
        }

        const float score =
            p.canal_w_flow * drop - p.canal_w_dig * excavate + water_pull -
            w_turn_eff * static_cast<float>(turn_steps(dir, d)) * kQuarterPi;
        if (chosen < 0 || score > best) {
          best = score;
          chosen = d;
        }
      }
      if (chosen < 0) {
        if (offmap_dir >= 0) {
          out.trail_dir.data[static_cast<size_t>(cell)] = static_cast<uint8_t>(offmap_dir);
          end = CanalEnd::LeftMap;
        } else {
          end = CanalEnd::Merged;  // hemmed in against the border; a terminus
        }
        break;
      }
      if (!survivors.empty() && rng.unit() < p.canal_wander_chance)
        chosen = survivors[rng.next() % survivors.size()];

      const int nx = cx + kDx[chosen], ny = cy + kDy[chosen];
      out.trail_dir.data[static_cast<size_t>(cell)] = static_cast<uint8_t>(chosen);
      if (nx < 0 || ny < 0 || nx >= w || ny >= ht) {
        end = CanalEnd::LeftMap;
        break;
      }
      const size_t next = static_cast<size_t>(ny) * w + nx;

      // TOUCHING WATER KILLS THE AGENT, and it does NOT carve that cell. One
      // rule doing three jobs: a tributary ends where it meets a trunk rather
      // than following and re-excavating it (which snowballed depth across
      // agents); an agent reaching a lake is absorbed; and an agent looping
      // back into its own trail closes a meander and leaves an island.
      if (out.trail_source.data[next] >= 0) {
        end = CanalEnd::Merged;
        break;
      }
      if (lake_mask.data[next]) {
        end = CanalEnd::Lake;
        break;
      }

      // Carve to a fixed depth below the BANKS — the cells either side of the
      // direction of travel, measured on the pre-canal terrain. Purely local,
      // so depth cannot accumulate along the path however far the canal runs.
      // A carried reference is what produced 15-19 m trenches and depth that
      // scaled with length.
      const int left = (chosen + 2) % 8, right = (chosen + 6) % 8;
      float bank = B0.data[next];
      for (const int side : {left, right}) {
        const int bx = nx + kDx[side], by = ny + kDy[side];
        if (bx >= 0 && by >= 0 && bx < w && by < ht)
          bank = std::min(bank, B0.data[static_cast<size_t>(by) * w + bx]);
      }
      const float bed = bank - p.canal_depth_m;
      const float before = B.data[next];
      if (before > bed) {
        B.data[next] = bed;
        out.stats.total_excavated_m += before - bed;
        out.stats.max_carve_m = std::max(out.stats.max_carve_m, before - bed);
      }

      out.trail_source.data[next] = agent;
      dir = chosen;
      cell = static_cast<int>(next);
    }
    out.stats.longest_path = std::max(out.stats.longest_path, steps);
    ++out.stats.ends[static_cast<int>(end)];
  }
  return out;
}

}  // namespace badlands::mapgen
