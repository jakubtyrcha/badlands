#pragma once

// Shared CPU thread pool + a blocking parallel-for, backed by a process-wide
// Taskflow executor (the multithreading setup ported from sampo). The taskflow
// dependency stays in parallel.cpp so callers only see this narrow API.
#include <cstddef>
#include <functional>

namespace badlands {

// Number of worker threads in the global pool (>= 1).
unsigned GetWorkerThreadCount();

// Runs body(i) for every i in [0, count) across the global thread pool,
// blocking until all complete. `body` MUST be thread-safe: distinct i values
// run concurrently, so it must not touch shared mutable state without its own
// synchronization (writing disjoint output slices is the intended pattern).
// count == 0 is a no-op.
//
// DO NOT CALL THIS FROM INSIDE A `body`. It blocks until the whole range
// finishes, and a worker that blocks parks instead of helping, so a nested call
// can deadlock. Structure nested work as self-contained items instead: the
// terrain cluster build fans out over tiles and runs each tile's own reduction
// serially inside the tile, which is both deadlock-free and faster than nesting
// would have been.
//
// Scheduling is DYNAMIC at chunk size 1 — one item per worker per grab. This
// matters whenever per-item cost varies, and both cheaper strategies were tried
// and measured on the terrain leaf pass (~350x spread between a plain tile and
// a river-refined one): a static chunk per worker strands the expensive items,
// and guided (Taskflow's default) still hands out LARGE chunks first, which
// measured 4.2x on 17 workers where chunk-1 reached 11.8x. Do not "optimize"
// this back to bigger chunks without re-measuring that case.
void ParallelFor(size_t count, const std::function<void(size_t)>& body);

}  // namespace badlands
