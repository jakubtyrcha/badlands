// Shared thread pool + parallel-for — see parallel.hpp. Backed by a global
// Taskflow executor (the MT setup ported from sampo's background_tasks).
#include "core/parallel.hpp"

#include <algorithm>
#include <thread>

#include <taskflow/taskflow.hpp>
// taskflow.hpp carries the core graph only; the parallel algorithms are opt-in
// headers, and for_each_index is a template that must be visible where used.
#include <taskflow/algorithm/for_each.hpp>

namespace badlands {

namespace {

// One executor for the whole process, sized to leave a core for the main
// thread. Constructed on first use; its worker threads live until program exit
// (mirrors sampo's BackgroundTaskSystem executor).
tf::Executor& GlobalExecutor() {
  static tf::Executor executor([] {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 1 ? hw - 1 : 1;
  }());
  return executor;
}

}  // namespace

unsigned GetWorkerThreadCount() {
  return static_cast<unsigned>(GlobalExecutor().num_workers());
}

void ParallelFor(size_t count, const std::function<void(size_t)>& body) {
  if (count == 0) return;

  // DYNAMIC partitioning at chunk size 1: every worker takes one item at a time.
  //
  // Two rejected alternatives, both measured on the terrain leaf pass, whose
  // per-item cost spans ~350x (a plain tile vs a river-refined one):
  //   - one contiguous chunk per worker (the original): lands most of the
  //     expensive items in a couple of chunks and the whole call waits on them.
  //   - guided (Taskflow's default): chunks START large, so the first worker to
  //     grab one can swallow several expensive items at once. Measured 4.2x on
  //     17 workers where the work supported far more; chunk-1 reached 11.8x.
  // Chunk size 1 costs one relaxed atomic per item, which is noise against any
  // body worth parallelizing — and this API is only for bodies worth it.
  tf::Taskflow flow;
  flow.for_each_index(size_t(0), count, size_t(1),
                      [&body](size_t i) { body(i); }, tf::DynamicPartitioner(1));
  GlobalExecutor().run(flow).wait();
}

}  // namespace badlands
