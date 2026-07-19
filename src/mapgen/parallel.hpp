#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace badlands::mapgen {

// Tiled parallel-for over a width x height domain. The domain is split into
// square tiles of `tile` pixels; tiles are pulled off a shared atomic counter
// by a fixed pool of worker threads (work-stealing over coarse tiles).
//
// `make_local()` is invoked ONCE per worker thread and its result is passed by
// reference to every tile that worker processes — this is how a per-thread
// noiser ExecutionContext is created once and reused across a thread's tiles
// (VM contexts are not shareable across threads). For passes with no per-thread
// state, return a trivial Local (e.g. std::monostate{}).
//
// `body(local, x0, y0, x1, y1)` processes the half-open tile [x0,x1) x [y0,y1).
// Tiles are disjoint, so bodies may write their pixels without synchronization.
template <typename MakeLocal, typename Body>
void parallel_tiles(int width, int height, int tile, MakeLocal make_local,
                    Body body) {
  if (width <= 0 || height <= 0) return;
  tile = std::max(1, tile);

  std::vector<std::pair<int, int>> origins;
  for (int y0 = 0; y0 < height; y0 += tile)
    for (int x0 = 0; x0 < width; x0 += tile) origins.emplace_back(x0, y0);

  std::atomic<size_t> next{0};
  unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  unsigned n = static_cast<unsigned>(
      std::min<size_t>(hw, std::max<size_t>(1, origins.size())));

  auto worker = [&]() {
    auto local = make_local();
    for (;;) {
      size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= origins.size()) break;
      const auto [x0, y0] = origins[i];
      const int x1 = std::min(x0 + tile, width);
      const int y1 = std::min(y0 + tile, height);
      body(local, x0, y0, x1, y1);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(n);
  for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
  for (auto& th : pool) th.join();
}

}  // namespace badlands::mapgen
