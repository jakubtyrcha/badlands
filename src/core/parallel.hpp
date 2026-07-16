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
void ParallelFor(size_t count, const std::function<void(size_t)>& body);

}  // namespace badlands
