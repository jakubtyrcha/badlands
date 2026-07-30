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
