#pragma once
#include <mutex>
#include <optional>
#include <thread>

#include "dcsdd.h"
#include "scene.h"

namespace sq {

// Pure coalescing state machine for background mesh reconstruction: tracks
// idle/running plus at most one pending snapshot. Deliberately separated
// from MeshJobRunner's threading (below, added by a later increment) so the
// four semantic rules are testable synchronously -- no thread timing
// involved (task-D6 brief).
//
// Semantics:
// - on_request while idle: transitions to running, returns the snapshot to
//   start now.
// - on_request while running: stores the snapshot as pending, replacing any
//   previous pending (only the LATEST edit matters) -- returns nothing.
// - on_complete with a pending snapshot: stays running, returns the pending
//   snapshot to start next (clearing it).
// - on_complete with no pending: goes idle, returns nothing.
class MeshJobQueue {
public:
    std::optional<SceneDocument> on_request(SceneDocument snapshot);
    std::optional<SceneDocument> on_complete();

    bool is_running() const { return running_; }

private:
    bool running_ = false;
    std::optional<SceneDocument> pending_;
};

// Threaded runner: drives MeshJobQueue under a mutex, running DCSDD
// reconstruction on a background thread. One thread is spawned per
// idle->running transition; while that thread is running, coalesced
// requests never spawn a new thread -- the running thread itself checks
// MeshJobQueue::on_complete() after finishing a job and, if a pending
// snapshot showed up while it worked, loops straight into the next job on
// the SAME thread (see mesh_job.cpp's run_loop). This is simpler than a
// persistent worker + condition variable: no thread sits idle waiting for
// work, and there is never more than one worker thread alive at a time (the
// previous one has always finished -- run_loop only returns once the queue
// is idle -- before request() spawns the next one, so joining it is
// non-blocking in practice; see MeshJobRunner::request).
//
// No Metal anywhere in this module.
class MeshJobRunner {
public:
    explicit MeshJobRunner(DcsddConfig config);
    ~MeshJobRunner();

    MeshJobRunner(const MeshJobRunner&) = delete;
    MeshJobRunner& operator=(const MeshJobRunner&) = delete;

    // Main thread, called after any scene mutation. Starts a job immediately
    // if idle; otherwise coalesces into the pending slot (only the latest
    // snapshot survives -- see MeshJobQueue).
    void request(SceneDocument snapshot);

    // Main thread, nonblocking. Returns true (and fills `out`) at most once
    // per newly finished mesh -- the LATEST one only, even if more than one
    // job finished since the last poll.
    bool poll(TriangleMesh& out);

private:
    void run_loop(SceneDocument doc); // background thread body

    DcsddConfig config_;

    // Guards queue_ (state transitions), latest_result_ (the mailbox), and
    // shutting_down_ -- every access to any of the three, from either the
    // main thread or the worker, holds this lock. See mesh_job.cpp for the
    // full thread-safety argument.
    std::mutex mutex_;
    MeshJobQueue queue_;
    std::optional<TriangleMesh> latest_result_;
    bool shutting_down_ = false;

    std::thread worker_; // the current (or most recently finished) job chain's thread
};

} // namespace sq
