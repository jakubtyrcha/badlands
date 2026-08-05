#include <doctest.h>

#include <cmath>
#include <simd/simd.h>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "gizmo.h"
#include "scene.h"

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

// Editor::create()'s camera rebuilt from the same literals it documents, with
// the aspect the 800x500 setViewportSize below produces — lets tests aim
// clicks at world-space gizmo handle points via Camera::project.
Camera editor_test_camera() {
    Camera cam;
    cam.eye = {4.0f, 3.0f, 6.0f};
    cam.target = {0.0f, 0.5f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fov_y_radians = 1.0472f;
    cam.aspect = 800.0f / 500.0f;
    return cam;
}

// The gizmo frame the editor uses for a node, rebuilt from public state (the
// snap fields of a snapped node come from the pick() that snapped it —
// spawn's snap raycast and pick share the same code path).
GizmoFrame frame_for(Editor* editor, int32_t node_id, bool snapped,
                     simd_float3 snap_point = {}, simd_float3 snap_normal = {0.0f, 1.0f, 0.0f}) {
    Node stub;
    stub.position = to_simd(editor->nodePosition(node_id));
    stub.snapped = snapped;
    stub.snap_point = snap_point;
    stub.snap_normal = snap_normal;
    return gizmo_frame_for_node(stub, editor_test_camera(), GizmoKind::Move);
}

// Projects a world point and returns its view coords, REQUIREing visibility.
struct ClickPoint { float x, y; };
ClickPoint click_at(simd_float3 world) {
    const ViewPoint vp = editor_test_camera().project(world, 800.0f, 500.0f);
    REQUIRE(vp.visible);
    return ClickPoint{vp.x, vp.y};
}

void check_float3_close(const simd_float3 actual, const simd_float3 expected, const float tol) {
    CHECK(std::fabs(actual.x - expected.x) < tol);
    CHECK(std::fabs(actual.y - expected.y) < tol);
    CHECK(std::fabs(actual.z - expected.z) < tol);
}

} // namespace

// --- Editor: drag-move integration (public surface only) -------------------
//
// Editor::create()'s camera (core/src/editor.cpp): eye {4,3,6}, target
// {0,0.5,0}, aspect replaced by setViewportSize below to 800/500=1.6 — the
// same camera picking_tests.cpp's Editor-integration test cross-validates
// against. camera_forward = normalize(target - eye) is re-derived from those
// same two literals rather than hardcoded, so the test stays correct if the
// camera setup ever changes.

TEST_CASE("Editor: PlaneUV drag moves an unsnapped node within the camera-orthogonal plane") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const simd_float3 eye = {4.0f, 3.0f, 6.0f};
    const simd_float3 target = {0.0f, 0.5f, 0.0f};
    const simd_float3 camera_forward = simd_normalize(target - eye);

    // Empty scene: the center-ray raycast misses everything, so this spawn
    // lands unsnapped at eye + dir*kUnsnappedSpawnDistance (scene.h) — the
    // gizmo frame for an unsnapped node is {node.position, -camera_forward}.
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    REQUIRE(spawned.snapped == false);
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection

    const GizmoFrame f = frame_for(editor, spawned.node_id, false);
    const float he = f.half_extent;
    const simd_float3 before = to_simd(editor->nodePosition(spawned.node_id));

    // Grab the u-v patch center (derived from the shared bounds, not a
    // literal -- those bounds moved once already), pull it 0.3he along u.
    const simd_float3 grab = f.origin + kGizmoPatchCenter * he * (f.u + f.v);
    const ClickPoint p1 = click_at(grab);
    const ClickPoint p2 = click_at(grab + 0.3f * he * f.u);
    CHECK(editor->beginDrag(p1.x, p1.y));
    editor->updateDrag(p2.x, p2.y);
    editor->endDrag();

    const simd_float3 after = to_simd(editor->nodePosition(spawned.node_id));
    const simd_float3 delta = after - before;

    check_float3_close(delta, 0.3f * he * f.u, 5e-3f);
    // Both the drag-start hit and every subsequent hit lie on the same
    // stored plane (normal == -camera_forward), so the delta between any two
    // of them is exactly in-plane, i.e. orthogonal to camera_forward.
    CHECK(std::fabs(simd_dot(delta, camera_forward)) < 1e-4f);

    SUBCASE("a second updateDrag after endDrag does nothing") {
        editor->updateDrag(200.0f, 450.0f);
        check_float3_approx(to_simd(editor->nodePosition(spawned.node_id)), after);
    }
}

TEST_CASE("Editor: AxisU drag constrains the move to the grabbed axis") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.snapped == false);
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection

    const GizmoFrame f = frame_for(editor, spawned.node_id, false);
    const float he = f.half_extent;
    const simd_float3 before = to_simd(editor->nodePosition(spawned.node_id));

    const ClickPoint p1 = click_at(f.origin + 0.8f * he * f.u);
    const ClickPoint p2 = click_at(f.origin + 1.2f * he * f.u);
    CHECK(editor->beginDrag(p1.x, p1.y));
    editor->updateDrag(p2.x, p2.y);
    editor->endDrag();

    const simd_float3 delta = to_simd(editor->nodePosition(spawned.node_id)) - before;
    check_float3_close(delta, 0.4f * he * f.u, 5e-3f);
    CHECK(std::fabs(simd_dot(delta, f.v)) < 5e-3f);
    CHECK(std::fabs(simd_dot(delta, f.n)) < 5e-3f);
}

TEST_CASE("Editor: beginDrag with no selection returns false and is a safe no-op") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    editor->select(kInvalidNode); // fresh editor already has no selection; explicit for clarity

    CHECK_FALSE(editor->beginDrag(400.0f, 250.0f));
    editor->updateDrag(500.0f, 250.0f); // must not crash; nothing to move
    editor->endDrag();

    CHECK(editor->selectedNode() == kInvalidNode);
}

TEST_CASE("Editor: off-handle click does not activate a drag") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    // Load-bearing: without a VISIBLE gizmo beginDrag would refuse for the
    // wrong reason (nothing drawn) and this case would stop testing the
    // off-handle rule it is named for.
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection
    const simd_float3 before = to_simd(editor->nodePosition(spawned.node_id));

    // Far corner: outside every handle (the gizmo spans ~±120pts around the
    // viewport center at kGizmoScreenFraction = 0.24 of 500pts). The margin
    // is wide either way, but the arithmetic here went stale when that
    // fraction doubled in R2 and is corrected rather than left misleading.
    CHECK_FALSE(editor->beginDrag(60.0f, 60.0f));
    editor->updateDrag(400.0f, 250.0f);
    editor->endDrag();

    check_float3_approx(to_simd(editor->nodePosition(spawned.node_id)), before);
}

TEST_CASE("Editor: snapped node — PlaneUV drag moves it in the snap plane; AxisN pull lifts "
          "it off the surface and the frame follows") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    // The snap fields spawn will capture: pick() and spawn's snap raycast
    // share the scene raycast, so picking the same view point first gives
    // the snap_point/snap_normal of the spawn that follows.
    const PickResult pr = editor->pick(400.0f, 250.0f);
    REQUIRE(pr.node_id == a.node_id);
    const SpawnResult b = editor->spawn(Shape::Sphere, Op::Subtract, 400.0f, 250.0f);
    REQUIRE(b.snapped == true);
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection

    const GizmoFrame f = frame_for(editor, b.node_id, true, to_simd(pr.point), to_simd(pr.normal));
    const float he = f.half_extent;

    SUBCASE("PlaneUV drag stays in the snap plane") {
        const simd_float3 before = to_simd(editor->nodePosition(b.node_id));
        const simd_float3 grab = f.origin + kGizmoPatchCenter * he * (f.u + f.v);
        const ClickPoint p1 = click_at(grab);
        const ClickPoint p2 = click_at(grab + 0.3f * he * f.v);
        CHECK(editor->beginDrag(p1.x, p1.y));
        editor->updateDrag(p2.x, p2.y);
        editor->endDrag();

        const simd_float3 delta = to_simd(editor->nodePosition(b.node_id)) - before;
        check_float3_close(delta, 0.3f * he * f.v, 5e-3f);
        CHECK(std::fabs(simd_dot(delta, f.n)) < 5e-3f);
    }

    SUBCASE("AxisN pull moves along the surface normal; a second grab on the moved frame works") {
        const simd_float3 before = to_simd(editor->nodePosition(b.node_id));
        const ClickPoint p1 = click_at(f.origin + 0.8f * he * f.n);
        const ClickPoint p2 = click_at(f.origin + 1.2f * he * f.n);
        CHECK(editor->beginDrag(p1.x, p1.y));
        editor->updateDrag(p2.x, p2.y);
        editor->endDrag();

        const simd_float3 delta = to_simd(editor->nodePosition(b.node_id)) - before;
        check_float3_close(delta, 0.4f * he * f.n, 5e-3f);

        // The frame rides rigidly with the node (snap_point += delta), so the
        // same handle-relative grab on the NEW frame must activate again —
        // this is the observable proof snap_point tracked the move.
        const GizmoFrame f2 = frame_for(editor, b.node_id, true,
                                        to_simd(pr.point) + delta, to_simd(pr.normal));
        const ClickPoint p3 = click_at(f2.origin + 0.8f * f2.half_extent * f2.n);
        CHECK(editor->beginDrag(p3.x, p3.y));
        editor->endDrag();
    }
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

    // A: unsnapped cube at the center ray; beginDrag on A's u-v patch
    // captures A's frame and start state while A is selected.
    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection
    const GizmoFrame fa = frame_for(editor, a.node_id, false);
    const ClickPoint pa = click_at(fa.origin + 0.45f * fa.half_extent * (fa.u + fa.v));
    REQUIRE(editor->beginDrag(pa.x, pa.y));

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

// --- Editor: gizmo hover -----------------------------------------------------

TEST_CASE("Editor: updateGizmoHover with no viewport or no selection stays None") {
    Editor* editor = Editor::create();
    editor->updateGizmoHover(400.0f, 250.0f); // zero viewport
    CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);

    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    editor->updateGizmoHover(400.0f, 250.0f); // no selection
    CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
}

TEST_CASE("Editor: gizmo hover tracks handles and never outlives the gizmo it points at") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection

    const GizmoFrame f = frame_for(editor, spawned.node_id, false);
    const ClickPoint on_u = click_at(f.origin + 0.8f * f.half_extent * f.u);

    editor->updateGizmoHover(on_u.x, on_u.y);
    CHECK(editor->gizmoHoverHandle() == GizmoHandle::AxisU);

    SUBCASE("moving off every handle resets to None") {
        editor->updateGizmoHover(60.0f, 60.0f);
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }

    SUBCASE("clearGizmoHover resets") {
        editor->clearGizmoHover();
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }

    SUBCASE("hiding the gizmo clears hover, and hover stays None while hidden") {
        editor->setGizmoVisible(false);
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
        editor->updateGizmoHover(on_u.x, on_u.y);
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }

    SUBCASE("selection change clears hover") {
        editor->select(kInvalidNode);
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }

    SUBCASE("deleting the selected node clears hover (deletion bypasses select())") {
        editor->deleteSelectedNode();
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }

    SUBCASE("endDrag clears hover — a drag can move the gizmo out from under the cursor, "
            "so the pre-drag hover is stale (post-R3 review finding)") {
        REQUIRE(editor->beginDrag(on_u.x, on_u.y));
        editor->updateDrag(on_u.x + 40.0f, on_u.y);
        editor->endDrag();
        CHECK(editor->gizmoHoverHandle() == GizmoHandle::None);
    }
}

TEST_CASE("Editor: a hidden gizmo has no grabbable handles either — you cannot drag "
          "what is not drawn") {
    // Regression, branch review: beginDrag checked viewport and selection but
    // NOT gizmo visibility, while updateGizmoHover right above it did. The
    // asymmetry became reachable when the app started hiding the gizmo while
    // the radial menu is on Scale: the handles are gone from the screen, so a
    // click at their old position must not still grab one.
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    editor->setGizmoVisible(true);

    const GizmoFrame f = frame_for(editor, spawned.node_id, false);
    const ClickPoint on_u = click_at(f.origin + 0.8f * f.half_extent * f.u);
    const simd_float3 before = to_simd(editor->nodePosition(spawned.node_id));

    // Sanity: the point really is on a handle while the gizmo is shown.
    REQUIRE(editor->beginDrag(on_u.x, on_u.y));
    editor->endDrag();

    SUBCASE("hidden: the same click is inert and the node does not move") {
        editor->setGizmoVisible(false);
        CHECK_FALSE(editor->beginDrag(on_u.x, on_u.y));
        // updateDrag must be a no-op too, not resume a drag that never began.
        editor->updateDrag(on_u.x + 60.0f, on_u.y + 40.0f);
        editor->endDrag();
        check_float3_approx(to_simd(editor->nodePosition(spawned.node_id)), before);
    }

    SUBCASE("shown again: the handle is grabbable once more") {
        editor->setGizmoVisible(false);
        editor->setGizmoVisible(true);
        CHECK(editor->beginDrag(on_u.x, on_u.y));
        editor->endDrag();
    }
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
    editor->setGizmoVisible(true); // the VM does this in modify mode with a selection

    // Pull along +u, which projects screen-LEFT for this camera (u == -camera
    // right for an unsnapped frame): the anchor must follow below x=400.
    const GizmoFrame f = frame_for(editor, spawned.node_id, false);
    const ClickPoint p1 = click_at(f.origin + 0.8f * f.half_extent * f.u);
    const ClickPoint p2 = click_at(f.origin + 1.2f * f.half_extent * f.u);
    REQUIRE(editor->beginDrag(p1.x, p1.y));
    editor->updateDrag(p2.x, p2.y);
    editor->endDrag();

    const ScreenPoint anchor = editor->projectSelectedAnchor();
    CHECK(anchor.visible);
    CHECK(anchor.x < 400.0f); // anchor moved the same direction as the drag
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
