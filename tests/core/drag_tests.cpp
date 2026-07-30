#include <doctest.h>

#include <cmath>
#include <simd/simd.h>

#include <shapeshifter/ShapeshifterCore.h>

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in lines_tests.cpp/picking_tests.cpp/scene_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

simd_float3 to_simd(Vec3f v) { return simd_float3{v.x, v.y, v.z}; }

} // namespace

// --- Editor: drag-move integration (public surface only) -------------------
//
// Editor::create()'s camera (core/src/editor.cpp): eye {4,3,6}, target
// {0,0.5,0}, aspect replaced by setViewportSize below to 800/500=1.6 — the
// same camera picking_tests.cpp's Editor-integration test cross-validates
// against. camera_forward = normalize(target - eye) is re-derived from those
// same two literals rather than hardcoded, so the test stays correct if the
// camera setup ever changes.

TEST_CASE("Editor: beginDrag/updateDrag/endDrag moves an unsnapped node "
          "within the camera-orthogonal plane") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const simd_float3 eye = {4.0f, 3.0f, 6.0f};
    const simd_float3 target = {0.0f, 0.5f, 0.0f};
    const simd_float3 camera_forward = simd_normalize(target - eye);

    // Empty scene: the center-ray raycast misses everything, so this spawn
    // lands unsnapped at eye + dir*kUnsnappedSpawnDistance (scene.h) — the
    // drag plane for an unsnapped node is {node.position, -camera_forward}.
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    REQUIRE(spawned.snapped == false);

    const simd_float3 before = to_simd(editor->nodePosition(spawned.node_id));

    editor->beginDrag(400.0f, 250.0f);
    editor->updateDrag(500.0f, 250.0f);
    editor->endDrag();

    const simd_float3 after = to_simd(editor->nodePosition(spawned.node_id));
    const simd_float3 delta = after - before;

    CHECK(simd_length(delta) > 1e-4f); // the drag actually moved the node
    // Both the drag-start hit and every subsequent hit lie on the same
    // stored plane (normal == -camera_forward), so the delta between any two
    // of them is exactly in-plane, i.e. orthogonal to camera_forward.
    CHECK(std::fabs(simd_dot(delta, camera_forward)) < 1e-4f);

    SUBCASE("a second updateDrag after endDrag does nothing") {
        editor->updateDrag(200.0f, 450.0f);
        check_float3_approx(to_simd(editor->nodePosition(spawned.node_id)), after);
    }
}

TEST_CASE("Editor: beginDrag with no selection is a safe no-op") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    editor->select(kInvalidNode); // fresh editor already has no selection; explicit for clarity

    editor->beginDrag(400.0f, 250.0f);
    editor->updateDrag(500.0f, 250.0f); // must not crash; nothing to move
    editor->endDrag();

    CHECK(editor->selectedNode() == kInvalidNode);
}

TEST_CASE("Editor: dragging a snapped node moves it without crashing") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    // Second spawn at the same viewport point re-raycasts and hits cube A,
    // so it snaps onto it (matches picking_tests.cpp's analogous case).
    const SpawnResult b = editor->spawn(Shape::Sphere, Op::Subtract, 400.0f, 250.0f);
    REQUIRE(b.snapped == true);

    const simd_float3 before = to_simd(editor->nodePosition(b.node_id));

    editor->beginDrag(400.0f, 250.0f);
    editor->updateDrag(450.0f, 300.0f);
    editor->endDrag();

    const simd_float3 after = to_simd(editor->nodePosition(b.node_id));
    CHECK(simd_length(after - before) > 1e-4f);
}

// --- Regression: stale drag state must not leak across a selection change --
//
// Review finding on the M6 drag machinery: a caller can legally drive
// beginDrag() for node A and then, without an interleaving endDrag(),
// change the selection to a different node B (e.g. Swift used to let a mode
// key mid-drag skip endDrag(), then the modify-awaiting-selection pick path
// moved selection to B). A later updateDrag() must NOT apply A's captured
// plane/start_pos to B — that would silently teleport B by A's delta.
// Editor::updateDrag now guards this directly (impl_->drag.node_id must
// still equal the current selection), independent of any Swift-side fix, so
// this test exercises the core defense-in-depth path through the public
// surface alone.
TEST_CASE("Editor: updateDrag ignores a stale drag left active across a selection change "
          "onto a different node") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    // A: unsnapped cube at the center ray; beginDrag captures A's plane and
    // start state while A is selected.
    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    editor->beginDrag(400.0f, 250.0f);

    // Selection moves to a second, unrelated node B with no endDrag() in
    // between — spawning always selects the new node (Editor::spawn), same
    // effect as a select-mode pick landing on a different object.
    const SpawnResult b = editor->spawn(Shape::Sphere, Op::Subtract, 100.0f, 100.0f); // far corner: misses A, unsnapped
    REQUIRE(b.snapped == false);
    REQUIRE(editor->selectedNode() == b.node_id);

    const simd_float3 a_before = to_simd(editor->nodePosition(a.node_id));
    const simd_float3 b_before = to_simd(editor->nodePosition(b.node_id));

    editor->updateDrag(500.0f, 250.0f); // must be a no-op: selected (B) != the drag's captured node (A)

    check_float3_approx(to_simd(editor->nodePosition(a.node_id)), a_before); // A untouched (not selected)
    check_float3_approx(to_simd(editor->nodePosition(b.node_id)), b_before); // B did NOT jump by A's delta

    editor->endDrag();
}
