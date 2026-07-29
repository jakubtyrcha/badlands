#include "mapgen/canal_carve.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/hydrology.hpp"

namespace badlands::mapgen {

int32_t SourceSets::add() {
  const int32_t id = static_cast<int32_t>(parent_.size());
  parent_.push_back(id);
  return id;
}

int32_t SourceSets::find(int32_t id) {
  // Path compression. Iterative rather than recursive: a long merge chain
  // should not risk the stack.
  int32_t root = id;
  while (parent_[static_cast<size_t>(root)] != root)
    root = parent_[static_cast<size_t>(root)];
  while (parent_[static_cast<size_t>(id)] != root) {
    const int32_t next = parent_[static_cast<size_t>(id)];
    parent_[static_cast<size_t>(id)] = root;
    id = next;
  }
  return root;
}

void SourceSets::merge(int32_t a, int32_t b) {
  const int32_t ra = find(a), rb = find(b);
  if (ra == rb) return;
  // By SMALLER root, not by rank or size: the representative must not depend
  // on the order unions happen to be applied, or agent processing order would
  // leak into which id every cell resolves to.
  const int32_t lo = std::min(ra, rb), hi = std::max(ra, rb);
  parent_[static_cast<size_t>(hi)] = lo;
}

namespace {

// Compass directions in CIRCULAR order from +x, so index arithmetic mod 8 is
// angular arithmetic. hydrology.cpp's dx8/dy8 are grouped orthogonals-then-
// diagonals and are NOT circular, so they cannot be reused here. Declared in
// the header because trail_dir indexes into them.
constexpr const int* kDx = kCanalDx;
constexpr const int* kDy = kCanalDy;
constexpr float kQuarterPi = 0.78539816339f;

bool is_diagonal(int d) { return kDx[d] != 0 && kDy[d] != 0; }
float step_len_of(int d, float texel_m) {
  return texel_m * (is_diagonal(d) ? 1.41421356f : 1.0f);
}

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
  const size_t n = B.data.size();
  out.trail_discharge_m3_s = Field2D<float>(w, ht, 0.0f);
  out.trail_source = Field2D<int32_t>(w, ht, -1);
  if (p.canal_seed_area_m2 <= 0.0f || p.canal_slope <= 0.0f) return out;

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

  // Reference discharge for the meander scaling, DERIVED rather than a
  // parameter so it cannot drift out of step with the seeding threshold.
  const float q_ref = std::max(p.runoff_m_per_s * p.canal_seed_area_m2, 1e-12f);
  const int max_turn = std::clamp(
      static_cast<int>(std::lround(p.canal_max_turn_angle_rad / kQuarterPi)), 1, 3);
  const float sense = p.canal_sense_distance_texels;

  // One shared stamp array rather than a per-agent visited set: agents run
  // sequentially, so holding the agent id gives O(1) lookup and no per-agent
  // allocation.
  std::vector<int32_t> visit_stamp(n, -1);
  // 0xFF = "no onward direction". A trail's LAST cell never gets an outgoing
  // direction — the laying agent stamps it on the cell it leaves, then
  // terminates — so without a sentinel that cell would read as 0 (east) and
  // send a following agent stepping blindly off the trunk.
  out.trail_dir = Field2D<uint8_t>(w, ht, kCanalNoDir);
  Field2D<uint8_t>& trail_dir = out.trail_dir;
  std::vector<int> survivors;

  for (const int seed_cell : seeds) {
    const int32_t agent = out.sets.add();
    Rng rng(seed, static_cast<uint32_t>(seed_cell));
    ++out.stats.agents;

    int dir = 0;  // start heading downstream, from the routing already computed
    const int32_t rcv = r.receiver[static_cast<size_t>(seed_cell)];
    if (rcv >= 0) {
      const int dx = rcv % w - seed_cell % w, dy = rcv / w - seed_cell / w;
      for (int d = 0; d < 8; ++d)
        if (kDx[d] == dx && kDy[d] == dy) dir = d;
    }

    int cell = seed_cell;
    float q = p.runoff_m_per_s * area.data[static_cast<size_t>(seed_cell)];
    float ref = B.data[static_cast<size_t>(cell)];
    bool on_trail = false;
    // Did this agent enter `cell` as virgin ground? Only then does it own the
    // cell's outgoing direction. Stamping unconditionally lets an agent that
    // steers over its OWN network's cells (same root, so no merge, so
    // on_trail stays false) overwrite their stored direction with its own
    // heading — which can leave a trunk cell pointing back UPHILL at its
    // parent.
    bool owns_cell = true;
    visit_stamp[static_cast<size_t>(cell)] = agent;
    CanalEnd end = CanalEnd::StepCap;
    int steps = 0;

    for (; steps < p.canal_max_steps; ++steps) {
      const int cx = cell % w, cy = cell / w;
      int chosen = -1;

      if (on_trail) {
        // Steering is OFF while on a trail. After merging, every cell ahead is
        // same-source, and same-source repulsion would shove the agent right
        // back off the trunk it just joined.
        chosen = trail_dir.data[static_cast<size_t>(cell)];
        if (chosen == kCanalNoDir) {
          // The trunk ends here — it left the map or was absorbed. Follow it
          // no further; stepping on would be a blind move onto whatever
          // happens to lie ahead.
          end = CanalEnd::BoxedIn;
          break;
        }
        // Rule 2 applies on this path TOO. Skipping it let a trunk lead an
        // agent onto a mountain and cut it down to the agent's plains-level
        // ref — measured as a 37 m trench on seed 1 before this check.
        const int tnx = cx + kDx[chosen], tny = cy + kDy[chosen];
        if (tnx >= 0 && tny >= 0 && tnx < w && tny < ht &&
            B.data[static_cast<size_t>(tny) * w + tnx] > ref + p.canal_max_climb_m) {
          end = CanalEnd::ClimbBlocked;
          break;
        }
      } else {
        const float w_turn_eff = p.canal_w_turn * std::sqrt(q / q_ref);
        float best = 0.0f;
        int offmap_dir = -1;
        survivors.clear();
        for (int t = -max_turn; t <= max_turn; ++t) {
          const int d = (dir + t + 8) % 8;
          const int nx = cx + kDx[d], ny = cy + kDy[d];
          if (nx < 0 || ny < 0 || nx >= w || ny >= ht) {
            // Off-map is a real destination — water leaves at base level — not
            // a candidate to discard. Remembering it is what stops an agent
            // that has reached the border from reporting BoxedIn: the LeftMap
            // check below the scoring can never fire for a direction that was
            // filtered out before it could be chosen.
            if (offmap_dir < 0) offmap_dir = d;
            continue;
          }
          const size_t j = static_cast<size_t>(ny) * w + nx;
          if (visit_stamp[j] == agent) continue;                        // Rule 1
          if (B.data[j] > ref + p.canal_max_climb_m) continue;          // Rule 2
          survivors.push_back(d);

          const float ref_next = ref - p.canal_slope * step_len_of(d, texel_m);
          const float drop = B.data[static_cast<size_t>(cell)] - B.data[j];
          const float excavate = std::max(0.0f, B.data[j] - ref_next);

          // Water lookahead, signed by relative level and source-aware: below
          // attracts, above repels, and same-source repels outright — that is
          // the network folding back on itself, not a confluence.
          float water_pull = 0.0f;
          const int sx = cx + static_cast<int>(std::lround(kDx[d] * sense));
          const int sy = cy + static_cast<int>(std::lround(kDy[d] * sense));
          if (sx >= 0 && sy >= 0 && sx < w && sy < ht) {
            const size_t si = static_cast<size_t>(sy) * w + sx;
            const int32_t ssrc = out.trail_source.data[si];
            if (ssrc >= 0 || lake_mask.data[si]) {
              // find(), never a raw id compare — trail ids go stale on merge.
              if (ssrc >= 0 && out.sets.same(ssrc, agent)) {
                water_pull = -p.canal_w_water_m;
              } else {
                const float wd = B.data[static_cast<size_t>(cell)] - B.data[si];
                water_pull = p.canal_w_water_m *
                             std::clamp(wd / p.canal_water_falloff_m, -1.0f, 1.0f);
              }
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
          // Prefer staying on the map while any in-bounds step survives; only
          // once none does is running off the edge the answer.
          if (offmap_dir >= 0) {
            trail_dir.data[static_cast<size_t>(cell)] = static_cast<uint8_t>(offmap_dir);
            end = CanalEnd::LeftMap;
          } else {
            end = CanalEnd::BoxedIn;
          }
          break;
        }
        if (rng.unit() < p.canal_wander_chance)
          chosen = survivors[rng.next() % survivors.size()];
      }

      const int nx = cx + kDx[chosen], ny = cy + kDy[chosen];
      if (nx < 0 || ny < 0 || nx >= w || ny >= ht) {
        end = CanalEnd::LeftMap;
        break;
      }
      const size_t next = static_cast<size_t>(ny) * w + nx;
      if (visit_stamp[next] == agent) {  // Rule 1 again, for the on-trail path
        end = CanalEnd::BoxedIn;
        break;
      }

      // Record the outgoing direction only once the move is COMMITTED.
      // Writing it before the bounds and Rule-1 checks recorded steps the flow
      // never took, leaving cells pointing at a neighbour they never drained
      // to — including uphill.
      if (owns_cell || on_trail)
        trail_dir.data[static_cast<size_t>(cell)] = static_cast<uint8_t>(chosen);

      // Descend unconditionally, then cut. `ref` only ever falls and Rule 1
      // means each cell is entered at most once, so the profile ALONG the
      // channel is monotone decreasing by construction — the guarantee is
      // spatial, not merely temporal.
      ref -= p.canal_slope * step_len_of(chosen, texel_m);
      const float before = B.data[next];
      if (before > ref) {
        B.data[next] = ref;
        out.stats.total_excavated_m += before - ref;
        out.stats.max_carve_m = std::max(out.stats.max_carve_m, before - ref);
      }
      // `ref` FOLLOWS the ground down; it must not float above it. Without
      // this the descent guarantee only binds where the canal actually cuts:
      // over ground already below the reference nothing is carved, so the
      // canal simply follows the terrain — and the terrain can rise, putting
      // an uphill step in the channel. Clamping here makes the next step's
      // reference strictly below THIS cell's real height, so the profile is
      // monotone whether or not a cut happens.
      ref = std::min(ref, B.data[next]);
      if (B.data[next] > B.data[static_cast<size_t>(cell)] + 1e-6f)
        ++out.stats.uphill_carve_steps;  // the descent guarantee, at carve time

      // Merge BEFORE overwriting the trail, and across differing roots only:
      // adding discharge to same-source water would create water from nothing.
      const int32_t hit = out.trail_source.data[next];
      if (hit >= 0 && !out.sets.same(hit, agent)) {
        out.sets.merge(agent, hit);
        q += out.trail_discharge_m3_s.data[next];
        on_trail = true;
        ++out.stats.merges;
      } else if (hit >= 0 && !on_trail) {
        // Stepping onto our OWN network while steering freely — a fold-back,
        // which is what the source rule exists to prevent. Note the !on_trail
        // guard: once merged, every cell ahead IS same-source by construction,
        // so counting those would make this fire constantly on normal travel
        // rather than on the failure it is meant to catch.
        ++out.stats.merges_same_root;
      }

      visit_stamp[next] = agent;
      out.trail_source.data[next] = agent;
      out.trail_discharge_m3_s.data[next] =
          std::max(out.trail_discharge_m3_s.data[next], q);
      // Only stamp an outgoing direction on a VIRGIN cell; an existing trail
      // already knows where it goes. Leave it at the sentinel until this agent
      // actually leaves the cell.
      if (hit < 0) trail_dir.data[next] = kCanalNoDir;
      owns_cell = hit < 0;
      dir = chosen;
      cell = static_cast<int>(next);

      if (lake_mask.data[next]) {
        end = CanalEnd::Lake;
        break;
      }
    }
    out.stats.longest_path = std::max(out.stats.longest_path, steps);
    ++out.stats.ends[static_cast<int>(end)];
  }
  return out;
}

}  // namespace badlands::mapgen
