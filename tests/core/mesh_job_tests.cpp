#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include <shapeshifter/ShapeshifterCore.h>

#include "dcsdd.h"
#include "mesh_job.h"
#include "scene.h"
#include "sdf.h"

using namespace sq;

namespace {

// Distinguishable snapshots for the coalescing tests: node count tags which
// document a returned/pending snapshot is, so tests can tell them apart
// without needing SceneDocument equality.
SceneDocument make_doc_with_n_nodes(int32_t n) {
    SceneDocument doc;
    for (int32_t i = 0; i < n; ++i) {
        doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    }
    return doc;
}

// Polls `runner` in a sleep loop until a mesh arrives or `timeout` elapses.
// Generous but bounded per the brief -- CoreTests must never hang forever on
// a runner bug.
bool poll_with_timeout(MeshJobRunner& runner, TriangleMesh& out, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runner.poll(out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void check_mesh_bit_exact(const TriangleMesh& actual, const TriangleMesh& expected) {
    REQUIRE(actual.positions.size() == expected.positions.size());
    REQUIRE(actual.normals.size() == expected.normals.size());
    for (size_t i = 0; i < actual.positions.size(); ++i) {
        CHECK(actual.positions[i].x == expected.positions[i].x);
        CHECK(actual.positions[i].y == expected.positions[i].y);
        CHECK(actual.positions[i].z == expected.positions[i].z);
    }
    for (size_t i = 0; i < actual.normals.size(); ++i) {
        CHECK(actual.normals[i].x == expected.normals[i].x);
        CHECK(actual.normals[i].y == expected.normals[i].y);
        CHECK(actual.normals[i].z == expected.normals[i].z);
    }
}

} // namespace

// --- MeshJobQueue: pure coalescing state machine (no threads) --------------

TEST_CASE("MeshJobQueue: on_request while idle transitions to running and returns the snapshot") {
    MeshJobQueue queue;
    CHECK_FALSE(queue.is_running());

    std::optional<SceneDocument> started = queue.on_request(make_doc_with_n_nodes(1));

    REQUIRE(started.has_value());
    CHECK(started->nodes().size() == 1);
    CHECK(queue.is_running());
}

TEST_CASE("MeshJobQueue: on_request while running stores pending and returns nothing") {
    MeshJobQueue queue;
    queue.on_request(make_doc_with_n_nodes(1)); // now running

    std::optional<SceneDocument> result = queue.on_request(make_doc_with_n_nodes(2));

    CHECK_FALSE(result.has_value());
    CHECK(queue.is_running());
}

TEST_CASE("MeshJobQueue: on_complete with a pending snapshot stays running and returns it") {
    MeshJobQueue queue;
    queue.on_request(make_doc_with_n_nodes(1));
    queue.on_request(make_doc_with_n_nodes(2)); // pending = 2 nodes

    std::optional<SceneDocument> next = queue.on_complete();

    REQUIRE(next.has_value());
    CHECK(next->nodes().size() == 2);
    CHECK(queue.is_running());
}

TEST_CASE("MeshJobQueue: on_complete with no pending goes idle and returns nothing") {
    MeshJobQueue queue;
    queue.on_request(make_doc_with_n_nodes(1));

    std::optional<SceneDocument> next = queue.on_complete();

    CHECK_FALSE(next.has_value());
    CHECK_FALSE(queue.is_running());
}

TEST_CASE("MeshJobQueue: rapid requests while running keep only the newest pending") {
    MeshJobQueue queue;
    queue.on_request(make_doc_with_n_nodes(1)); // starts running with 1 node

    queue.on_request(make_doc_with_n_nodes(2));
    queue.on_request(make_doc_with_n_nodes(3));
    std::optional<SceneDocument> third_request_result = queue.on_request(make_doc_with_n_nodes(4));
    CHECK_FALSE(third_request_result.has_value());

    std::optional<SceneDocument> next = queue.on_complete();
    REQUIRE(next.has_value());
    CHECK(next->nodes().size() == 4); // only the LATEST of the three overwritten-pending survives
}

TEST_CASE("MeshJobQueue: full request -> complete -> pending-start -> complete cycle lands idle") {
    MeshJobQueue queue;
    std::optional<SceneDocument> first = queue.on_request(make_doc_with_n_nodes(1));
    REQUIRE(first.has_value());
    CHECK(queue.is_running());

    std::optional<SceneDocument> mid_result = queue.on_request(make_doc_with_n_nodes(2)); // pending
    CHECK_FALSE(mid_result.has_value());

    std::optional<SceneDocument> second = queue.on_complete(); // starts the pending snapshot
    REQUIRE(second.has_value());
    CHECK(second->nodes().size() == 2);
    CHECK(queue.is_running());

    std::optional<SceneDocument> done = queue.on_complete(); // no more pending
    CHECK_FALSE(done.has_value());
    CHECK_FALSE(queue.is_running());
}

// --- Latest-wins mailbox, simulated synchronously ---------------------------
//
// MeshJobRunner's mailbox (mutex-guarded latest-result slot, overwritten by
// each job completion, swapped out by poll()) is exercised for real only by
// the threaded integration test below. This test proves the *semantic*
// property -- "if two jobs finish between polls, the older result is
// discarded" -- without any thread timing: a synchronous fake executor drives
// MeshJobQueue directly and writes to a plain local variable standing in for
// the mailbox, exactly as MeshJobRunner's run loop would, but on one thread.
TEST_CASE("mailbox: latest-wins when two jobs complete before a poll (synchronous fake executor)") {
    MeshJobQueue queue;
    std::optional<int32_t> mailbox; // fake "result" slot tagged by node count

    std::optional<SceneDocument> job_a = queue.on_request(make_doc_with_n_nodes(1));
    REQUIRE(job_a.has_value());

    // Two more edits land while job A is still "running" -- only the newest
    // (3) survives as pending.
    queue.on_request(make_doc_with_n_nodes(2));
    queue.on_request(make_doc_with_n_nodes(3));

    // Job A "finishes": publish its result to the mailbox (nobody has polled
    // yet), then immediately pick up the pending job -- no poll happens in
    // between, exactly the race the brief calls out.
    mailbox = static_cast<int32_t>(job_a->nodes().size()); // 1
    std::optional<SceneDocument> job_b = queue.on_complete();
    REQUIRE(job_b.has_value());
    CHECK(job_b->nodes().size() == 3);

    // Job B finishes too, overwriting the mailbox before any poll read it.
    mailbox = static_cast<int32_t>(job_b->nodes().size()); // 3, overwrites 1
    std::optional<SceneDocument> idle = queue.on_complete();
    CHECK_FALSE(idle.has_value());
    CHECK_FALSE(queue.is_running());

    // "Poll": only the latest (3) is visible; job A's result (1) was
    // silently discarded, per the brief's latest-wins mailbox rule.
    REQUIRE(mailbox.has_value());
    CHECK(*mailbox == 3);
}

// --- MeshJobRunner: threaded integration (bounded, tiny) --------------------

TEST_CASE("MeshJobRunner: one-sphere scene reconstructs to a bit-exact match of direct reconstruct(); "
          "then an empty request clears the mesh") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Sphere, Op::Add, {0.0f, 0.0f, 0.0f});

    // Tiny/fast config per the brief -- not testing reconstruction quality
    // here (D5 already owns that), just that the runner gets a job onto a
    // background thread and back.
    DcsddConfig config;
    config.resolution = 12;
    config.outer_iters = 5;
    config.inner_iters = 5;

    MeshJobRunner runner(config);
    runner.request(doc);

    TriangleMesh mesh;
    REQUIRE(poll_with_timeout(runner, mesh, std::chrono::seconds(30)));
    REQUIRE_FALSE(mesh.positions.empty());

    // D5's determinism guarantee: reconstruct() on the same input is
    // bit-identical regardless of thread scheduling, so the runner's result
    // must match a direct, synchronous call exactly.
    const SampleGrid grid = sample_scene(doc, config.resolution);
    const TriangleMesh expected = reconstruct(grid, config);
    check_mesh_bit_exact(mesh, expected);

    // Clear path: an empty scene's job is sample_scene -> empty grid ->
    // empty TriangleMesh, skipping reconstruct() entirely (dcsdd_init
    // requires grid.n >= 2, which an empty-scene grid does not have).
    SceneDocument empty_doc;
    runner.request(empty_doc);

    TriangleMesh cleared;
    REQUIRE(poll_with_timeout(runner, cleared, std::chrono::seconds(30)));
    CHECK(cleared.positions.empty());
    CHECK(cleared.normals.empty());
}

TEST_CASE("MeshJobRunner: destructor waits for an in-flight job instead of leaking the thread") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Sphere, Op::Add, {0.0f, 0.0f, 0.0f});

    DcsddConfig config;
    config.resolution = 12;
    config.outer_iters = 5;
    config.inner_iters = 5;

    const auto start = std::chrono::steady_clock::now();
    {
        MeshJobRunner runner(config);
        runner.request(doc); // job starts on a background thread; destroyed below with no poll() --
                              // the destructor races (and must win against) the in-flight job.
    } // ~MeshJobRunner: signal shutting_down_, then join -- must return only once that job finishes.
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Reaching this line at all is the real assertion: the destructor did
    // not hang forever, crash, or leak the thread (a joinable std::thread
    // destroyed while still joinable calls std::terminate). The bound is a
    // generous sanity check -- this tiny grid's job normally finishes in
    // well under a second, per the runner integration test above.
    CHECK(elapsed < std::chrono::seconds(30));
}
