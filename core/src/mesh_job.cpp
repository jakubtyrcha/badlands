#include "mesh_job.h"

#include <utility>

namespace sq {

std::optional<SceneDocument> MeshJobQueue::on_request(SceneDocument snapshot) {
    if (!running_) {
        running_ = true;
        return snapshot;
    }
    // Overwrite any previous pending -- only the latest edit matters.
    pending_ = std::move(snapshot);
    return std::nullopt;
}

std::optional<SceneDocument> MeshJobQueue::on_complete() {
    if (pending_.has_value()) {
        std::optional<SceneDocument> next = std::move(pending_);
        pending_.reset();
        return next; // stays running_ (already true)
    }
    running_ = false;
    return std::nullopt;
}

namespace {

// One job: sample_scene -> (empty grid -> empty mesh) | reconstruct. An
// empty scene's grid is default-constructed (n == 0, see sdf.h) -- calling
// reconstruct/dcsdd_init on that would violate their n >= 2 precondition, so
// the empty case short-circuits here rather than falling through. This is
// the "clear the mesh" path: an empty TriangleMesh (both vectors empty) is
// exactly what an empty scene should render as.
TriangleMesh run_reconstruction(const SceneDocument& doc, const DcsddConfig& config) {
    const SampleGrid grid = sample_scene(doc, config.resolution);
    if (grid.n == 0) {
        return TriangleMesh{};
    }
    return reconstruct(grid, config);
}

} // namespace

MeshJobRunner::MeshJobRunner(DcsddConfig config) : config_(config) {}

MeshJobRunner::~MeshJobRunner() {
    // Signal no-new-work, then join. An in-flight reconstruct() cannot be
    // interrupted -- if a job is running, this blocks until it (and any
    // already-coalesced pending job it would otherwise have chained into,
    // see the shutting_down_ check in run_loop) finishes. Accepted per the
    // brief: app teardown kills the process regardless, and CoreTests only
    // ever destroys a runner after a tiny test-sized grid has finished (or
    // never requested one at all, in which case worker_ isn't joinable and
    // this returns immediately).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MeshJobRunner::request(SceneDocument snapshot) {
    std::optional<SceneDocument> to_start;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            return; // no new work once torn down
        }
        to_start = queue_.on_request(std::move(snapshot));
    }
    if (!to_start.has_value()) {
        return; // coalesced into pending; the running thread will pick it up
    }

    // queue_ was idle before this on_request call (that's the only case
    // on_request returns a value), so any previous worker_ has already run
    // its loop to completion and returned -- join is non-blocking here, just
    // reclaiming the std::thread object before reassigning it.
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread(&MeshJobRunner::run_loop, this, std::move(*to_start));
}

bool MeshJobRunner::poll(TriangleMesh& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_result_.has_value()) {
        return false;
    }
    out = std::move(*latest_result_);
    latest_result_.reset();
    return true;
}

void MeshJobRunner::run_loop(SceneDocument doc) {
    for (;;) {
        // Heavy work with no lock held -- request()/poll() on the main
        // thread must never block on an in-flight reconstruction.
        TriangleMesh mesh = run_reconstruction(doc, config_);

        std::optional<SceneDocument> next;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Overwrites any previous unpolled result -- if two jobs finish
            // between polls (possible here: this loop chains straight into
            // a coalesced pending job without an intervening poll), the
            // older result is silently discarded. Latest wins.
            latest_result_ = std::move(mesh);
            std::optional<SceneDocument> pending = queue_.on_complete();
            if (!shutting_down_) {
                next = std::move(pending);
            }
            // If shutting_down_, deliberately drop `pending` even though
            // on_complete() already consumed it from the queue: the
            // destructor is waiting to join this thread and no further
            // request()/poll() calls can happen once it returns, so
            // starting one more job would just be wasted (unobservable)
            // work that delays teardown.
        }
        if (!next.has_value()) {
            return; // idle: no pending work (or we're shutting down)
        }
        doc = std::move(*next);
    }
}

} // namespace sq
