#include "mapgen/hydrology.hpp"

#include <algorithm>
#include <queue>
#include <utility>

namespace badlands::mapgen {

FlowRouting route_flow(const Field2D<float>& h, float /*texel_m*/, float epsilon_m) {
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
  return r;
}

// Note the heap invariant: a neighbor is pushed with `wl >= level +
// epsilon_m > level`, so every cell pops after its receiver — `order` is
// topological by construction. This is load-bearing for every consumer that
// walks `order`: donors always process after (or, for the accumulate below,
// are folded into) their receiver.
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
