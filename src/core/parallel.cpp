// Shared thread pool + parallel-for — see parallel.hpp. Backed by a global
// Taskflow executor (the MT setup ported from sampo's background_tasks).
#include "core/parallel.hpp"

#include <algorithm>
#include <thread>

#include <taskflow/taskflow.hpp>

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
  tf::Executor& ex = GlobalExecutor();

  // Static chunking across workers — one task per worker covering a contiguous
  // range. Uses only stable Taskflow API (emplace + run().wait()), and keeps
  // task-scheduling overhead negligible vs. the per-item work.
  const size_t workers = std::max<size_t>(1, ex.num_workers());
  const size_t chunk = (count + workers - 1) / workers;

  tf::Taskflow flow;
  for (size_t w = 0; w < workers; ++w) {
    const size_t begin = w * chunk;
    if (begin >= count) break;
    const size_t end = std::min(begin + chunk, count);
    flow.emplace([&body, begin, end]() {
      for (size_t i = begin; i < end; ++i) body(i);
    });
  }
  ex.run(flow).wait();
}

}  // namespace badlands
