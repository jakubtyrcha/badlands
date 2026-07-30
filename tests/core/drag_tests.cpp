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

// --- Editor: radial-menu anchor projection ----------------------------------

TEST_CASE("Editor: projectSelectedAnchor round-trips the selected node's screen position, "
          "and reports invisible once the selection is cleared") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    // (400,250) is the exact center of an 800x500 viewport, so the
    // unsnapped spawn ray (through that same view point) lands the node
    // directly on the center ray. Projecting the node's position back
    // through the same camera/viewport must therefore return that same
    // screen point (within the brief's pinned 0.1pt tolerance).
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    REQUIRE(spawned.snapped == false);

    const ScreenPoint anchor = editor->projectSelectedAnchor();
    CHECK(anchor.visible);
    CHECK(std::fabs(anchor.x - 400.0f) < 0.1f);
    CHECK(std::fabs(anchor.y - 250.0f) < 0.1f);

    SUBCASE("clearing the selection makes the anchor invisible") {
        editor->select(kInvalidNode);
        CHECK_FALSE(editor->projectSelectedAnchor().visible);
    }
}

TEST_CASE("Editor: projectSelectedAnchor is invisible on a fresh editor (no viewport, no selection)") {
    Editor* editor = Editor::create();
    CHECK_FALSE(editor->projectSelectedAnchor().visible);
}

TEST_CASE("Editor: projectSelectedAnchor tracks the node through a drag") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.snapped == false);

    editor->beginDrag(400.0f, 250.0f);
    editor->updateDrag(460.0f, 250.0f); // drag right in view space
    editor->endDrag();

    const ScreenPoint anchor = editor->projectSelectedAnchor();
    CHECK(anchor.visible);
    CHECK(anchor.x > 400.0f); // anchor moved the same direction as the drag
}

// --- Editor: scale tool ------------------------------------------------------

TEST_CASE("Editor: beginScale/updateScale applies a cumulative exponential factor "
          "from the captured start scale") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), simd_float3{1.0f, 1.0f, 1.0f});

    editor->beginScale();

    // factor = exp(-pixelDeltaY * 0.005): updateScale(200) -> exp(-1.0) ~= 0.36788.
    editor->updateScale(200.0f);
    const float exp_neg1 = std::exp(-1.0f);
    check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), simd_float3{exp_neg1, exp_neg1, exp_neg1});

    // Cumulative from the captured START scale (still {1,1,1}), not from the
    // scale after the first update: updateScale(400) -> exp(-2.0) ~= 0.13534,
    // NOT exp(-1) applied again (which would give exp(-3), the incremental-
    // implementation bug this test guards against).
    editor->updateScale(400.0f);
    const float exp_neg2 = std::exp(-2.0f);
    check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), simd_float3{exp_neg2, exp_neg2, exp_neg2});

    editor->endScale();
}

TEST_CASE("Editor: updateScale clamps each scale component to [0.05, 50]") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    editor->beginScale();

    // exp(-1200*0.005) = exp(-6) ~= 0.00248 -> clamped up to the 0.05 floor.
    editor->updateScale(1200.0f);
    check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), simd_float3{0.05f, 0.05f, 0.05f});

    // Still cumulative from the same start scale ({1,1,1}):
    // exp(2000*0.005) = exp(10) ~= 22026 -> clamped down to the 50 ceiling.
    editor->updateScale(-2000.0f);
    check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), simd_float3{50.0f, 50.0f, 50.0f});

    editor->endScale();
}

TEST_CASE("Editor: updateScale is a safe no-op outside an active beginScale/endScale bracket") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    const simd_float3 unit_scale = {1.0f, 1.0f, 1.0f};

    SUBCASE("updateScale without a prior beginScale changes nothing") {
        editor->updateScale(200.0f);
        check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), unit_scale);
    }

    SUBCASE("beginScale with no selection is a safe no-op, and the following updateScale changes nothing") {
        editor->select(kInvalidNode);
        editor->beginScale(); // must not crash despite no selection
        editor->updateScale(200.0f);
        CHECK(editor->selectedNode() == kInvalidNode);
        check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), unit_scale);
    }

    SUBCASE("after endScale, further updateScale calls change nothing") {
        editor->beginScale();
        editor->updateScale(200.0f);
        editor->endScale();
        const simd_float3 after_end = to_simd(editor->nodeScale(spawned.node_id));

        editor->updateScale(400.0f);
        check_float3_approx(to_simd(editor->nodeScale(spawned.node_id)), after_end);
    }
}

TEST_CASE("Editor: pick still hits the node at its anchor after scaling up "
          "(indirect gizmo-follows-scale check)") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    editor->beginScale();
    editor->updateScale(-2000.0f); // scales up to the 50x ceiling, so max(scale) > 1
    editor->endScale();

    const ScreenPoint anchor = editor->projectSelectedAnchor();
    REQUIRE(anchor.visible);
    const PickResult picked = editor->pick(anchor.x, anchor.y);
    CHECK(picked.node_id == spawned.node_id);
}

// --- Editor: node op (menu toggle + color coding) ---------------------------

TEST_CASE("Editor: nodeOp/setNodeOp round-trip, and nodeOp defaults to Add for an unknown id") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    CHECK(editor->nodeOp(spawned.node_id) == Op::Add);
    editor->setNodeOp(spawned.node_id, Op::Subtract);
    CHECK(editor->nodeOp(spawned.node_id) == Op::Subtract);
    editor->setNodeOp(spawned.node_id, Op::Add);
    CHECK(editor->nodeOp(spawned.node_id) == Op::Add);

    CHECK(editor->nodeOp(99999) == Op::Add); // unknown id -> documented Add default

    // setNodeOp's effect on rendered vertex color (kColorAdd/kColorSubtract
    // in build_scene_lines) is already covered by lines_tests.cpp's op-color
    // assertions; no new assertion needed here beyond this state round-trip.
}

// --- Regression: stale scale state must not leak across a selection change -
//
// Mirror of the drag regression test above (same class of bug the M6 drag
// fix addressed): a caller can legally drive beginScale() for node A and
// then, without an interleaving endScale(), change the selection to a
// different node B. A later updateScale() must NOT apply A's captured
// start_scale to B.
TEST_CASE("Editor: updateScale ignores a stale scale gesture left active across a selection change "
          "onto a different node") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    editor->beginScale();

    // Selection moves to a second, unrelated node B with no endScale() in
    // between — spawning always selects the new node (Editor::spawn), same
    // effect as a radial-menu scale-tool switch landing on a different
    // object via a select-mode pick.
    const SpawnResult b = editor->spawn(Shape::Sphere, Op::Subtract, 100.0f, 100.0f); // far corner: misses A, unsnapped
    REQUIRE(b.snapped == false);
    REQUIRE(editor->selectedNode() == b.node_id);

    const simd_float3 a_before = to_simd(editor->nodeScale(a.node_id));
    const simd_float3 b_before = to_simd(editor->nodeScale(b.node_id));

    editor->updateScale(200.0f); // must be a no-op: selected (B) != the scale gesture's captured node (A)

    check_float3_approx(to_simd(editor->nodeScale(a.node_id)), a_before); // A untouched (not selected)
    check_float3_approx(to_simd(editor->nodeScale(b.node_id)), b_before); // B did NOT get rescaled by A's factor

    editor->endScale();
}
