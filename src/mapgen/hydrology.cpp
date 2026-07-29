#include "mapgen/hydrology.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

namespace badlands::mapgen {

FlowRouting route_flow(const Field2D<float>& h, float texel_m, float epsilon_m,
                       const Field2D<uint8_t>* lake_tag) {
  const int w = h.width, ht = h.height;
  FlowRouting r;
  r.width = w; r.height = ht;
  if (w <= 0 || ht <= 0) return r;
  const size_t n = h.size();
  r.receiver.assign(n, -1);
  r.in_lake.assign(n, 0);
  r.water_level.assign(n, 0.0f);
  r.order.reserve(n);
  std::vector<uint8_t> visited(n, 0);

  // min-heap on (level, linear index) — the index tie-break is the
  // determinism guarantee on flats
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  auto seed = [&](int x, int y) {
    const int i = y * w + x;
    if (visited[i]) return;
    visited[i] = 1;
    r.water_level[i] = h.data[i];
    pq.push({h.data[i], i});
  };
  for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, ht - 1); }
  for (int y = 0; y < ht; ++y) { seed(0, y); seed(w - 1, y); }

  static constexpr int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static constexpr int dy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!pq.empty()) {
    const auto [level, i] = pq.top();
    pq.pop();
    r.order.push_back(i);
    const int cx = i % w, cy = i / w;
    for (int k = 0; k < 8; ++k) {
      const int nx = cx + dx8[k], ny = cy + dy8[k];
      if (nx < 0 || ny < 0 || nx >= w || ny >= ht) continue;
      const int j = ny * w + nx;
      if (visited[j]) continue;
      visited[j] = 1;
      r.receiver[j] = i;
      const float wl = std::max(h.data[j], level + epsilon_m);
      r.water_level[j] = wl;
      r.in_lake[j] = wl > h.data[j] ? 1 : 0;
      pq.push({wl, j});
    }
  }

  // Pass 2: flow DIRECTION by steepest descent on the filled surface.
  //
  // Pass 1 fills depressions correctly, but its claim parent is the lowest
  // ALREADY-POPPED neighbor — a ranking by absolute elevation that ignores how
  // far away the neighbor is. On a surface tilted at theta a diagonal drops by
  // |cos|+|sin| against the best orthogonal's max(|cos|,|sin|), so the diagonal
  // is always at least as low and always wins the pop race. That made every
  // interior cell drain diagonally (measured: 100% on tilted planes at every
  // angle, 96.1% on the production sim), locking drainage to 45 degrees.
  // Steepest descent divides the drop by the distance, which cancels exactly
  // that bias.
  //
  // Ranking on hf = water_level rather than h is load-bearing three ways:
  //   - a strictly-lower neighbor always exists (a cell is pushed at
  //     wl >= its parent's pop level + epsilon, so the parent qualifies),
  //   - receivers strictly decrease hf, so the graph cannot cycle,
  //   - `order` is ascending in hf, so a receiver still pops before its
  //     donors and `order` remains topological — which incise, deposit and
  //     accumulate_drainage all depend on.
  // LAKE cells keep their pass-1 claim parent, and that exclusion is
  // load-bearing rather than an optimization. A lake surface is flat; the
  // epsilon tilt across it is bookkeeping to keep the graph acyclic, not a
  // physical slope. Running steepest descent on it invents downhill routes out
  // through the rim: the tilt lifts deep interior levels well above the sill
  // (a cell 1000 pops deep sits at spill + 0.1 m), and a dry rim cell is only
  // ever guaranteed to be above the level of whichever cell CLAIMED it, which
  // may have popped much earlier and lower. Measured on seed 2 with steepest
  // descent applied to lake cells: 42 of 113 components grew more than one
  // exit, worst case 18. The claim-parent tree instead funnels a component
  // along the flood tree toward its sill, which is the structure deposit's
  // lake pour was written against.
  //
  // WHICH cells count as lake is the caller's decision when `lake_tag` is
  // supplied, and that matters a great deal. Falling back on `in_lake` treats
  // every FLAT as a lake too — on level ground the flood front always arrives
  // above the cell's own height — which measured 33-45% of channel texels
  // routed by flood order rather than gradient. A caller-supplied tag marks
  // only resolved lakes, so flats route by gradient like any dry cell.
  //
  // The tag must mark WHOLE lakes, never merely the deep cells of one. Inside
  // a lake hf is nearly flat, so an untagged shallow margin can find a dry
  // neighbour below its epsilon-inflated level and reinstate exactly the
  // invented exits described above, at the shoreline.
  //
  // This exclusion removes the tilt-induced exits; it does NOT make components
  // single-exit, and they never were. 4-connected component labels against an
  // 8-connected receiver graph already produced multi-exit components under
  // the old routing (33 of 82 on seed 2). See find_exit in erosion.cpp for
  // what that costs.
  //
  // Both rules strictly decrease water_level, so acyclicity and the
  // topological `order` hold either way.
  const float diag_m = texel_m * std::sqrt(2.0f);
  const bool tagged = lake_tag != nullptr && lake_tag->data.size() == n;
  for (int i = 0; i < static_cast<int>(n); ++i) {
    if (r.receiver[i] < 0) continue;  // border: base level, keeps receiver -1
    const bool is_lake = tagged ? lake_tag->data[static_cast<size_t>(i)] != 0
                                : r.in_lake[i] != 0;
    if (is_lake) continue;  // lake: keep the flood tree, see above
    const int cx = i % w, cy = i / w;
    const float hf_i = r.water_level[i];
    int32_t best = r.receiver[i];  // defensive fallback: the pass-1 parent
    float best_slope = -1.0f;
    for (int k = 0; k < 8; ++k) {
      const int nx = cx + dx8[k], ny = cy + dy8[k];
      if (nx < 0 || ny < 0 || nx >= w || ny >= ht) continue;
      const int j = ny * w + nx;
      const float drop = hf_i - r.water_level[j];
      if (drop <= 0.0f) continue;  // strictly lower only
      const float slope = drop / ((dx8[k] != 0 && dy8[k] != 0) ? diag_m : texel_m);
      // Ties break on the smaller linear index, matching the heap's
      // (level, index) tie-break — this is the determinism guarantee on flats.
      if (slope > best_slope || (slope == best_slope && j < best)) {
        best_slope = slope;
        best = j;
      }
    }
    r.receiver[i] = best;
  }
  return r;
}

// Note why `order` is topological, which is load-bearing for every consumer
// that walks it (donors always process after — or, for the accumulate below,
// are folded into — their receiver). `order` is the pop order, ascending in
// `water_level`. Pass 2 only ever picks a receiver whose `water_level` is
// STRICTLY lower, so the receiver necessarily popped earlier. Note this no
// longer follows from the heap invariant alone (a neighbor being pushed at
// `wl >= level + epsilon_m`): that argued the pass-1 claim parent pops first,
// and pass 2 replaces the claim parent. The strict-descent rule is what
// carries the property now.
Field2D<float> accumulate_drainage(const FlowRouting& r, float texel_area_m2) {
  Field2D<float> a(r.width, r.height, texel_area_m2);
  for (size_t k = r.order.size(); k-- > 0;) {
    const int i = r.order[k];
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0) a.data[rcv] += a.data[i];
  }
  return a;
}

}  // namespace badlands::mapgen
