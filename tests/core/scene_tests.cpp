#include <doctest.h>

#include <string>

#include <shapeshifter/ShapeshifterCore.h>

#include "scene.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in lines_tests.cpp/picking_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Vec3f -> simd_float3, for asserting on Editor's public-surface node queries
// (nodePosition) with the same check_float3_approx helper above — matches
// drag_tests.cpp's to_simd.
simd_float3 to_simd(Vec3f v) { return simd_float3{v.x, v.y, v.z}; }

} // namespace

// --- naming / ids ----------------------------------------------------------

TEST_CASE("SceneDocument::spawn_unsnapped: auto-names by per-shape counter, ids by spawn order") {
    SceneDocument doc;

    const int32_t cube1 = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t sphere1 = doc.spawn_unsnapped(Shape::Sphere, Op::Subtract, {1.0f, 0.0f, 0.0f});
    const int32_t cube2 = doc.spawn_unsnapped(Shape::Cube, Op::Subtract, {2.0f, 0.0f, 0.0f});

    CHECK(cube1 == 1);
    CHECK(sphere1 == 2);
    CHECK(cube2 == 3);

    REQUIRE(doc.find(cube1) != nullptr);
    REQUIRE(doc.find(sphere1) != nullptr);
    REQUIRE(doc.find(cube2) != nullptr);

    CHECK(doc.find(cube1)->name == "Cube 1");
    CHECK(doc.find(sphere1)->name == "Sphere 1");
    CHECK(doc.find(cube2)->name == "Cube 2");

    CHECK(doc.find(cube1)->op == Op::Add);
    CHECK(doc.find(sphere1)->op == Op::Subtract);
    CHECK(doc.find(cube2)->op == Op::Subtract);
}

TEST_CASE("SceneDocument: per-shape name counters are independent of node ids") {
    // No removal API exists yet, so "independent of ids" is exercised by
    // interleaving shapes: the id sequence is contiguous (1,2,3) while each
    // shape's own counter only advances on its own spawns (Cube: 1,2;
    // Sphere: 1).
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t b = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t c = doc.spawn_unsnapped(Shape::Sphere, Op::Add, {0.0f, 0.0f, 0.0f});

    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);

    CHECK(doc.find(a)->name == "Cube 1");
    CHECK(doc.find(b)->name == "Cube 2");
    CHECK(doc.find(c)->name == "Sphere 1");
}

// --- spawn_snapped -----------------------------------------------------------

TEST_CASE("SceneDocument::spawn_snapped: axis-aligned normal places the node on the surface") {
    SceneDocument doc;

    const simd_float3 hit = {1.0f, 2.0f, 3.0f};
    const simd_float3 normal = {0.0f, 1.0f, 0.0f};
    const int32_t id = doc.spawn_snapped(Shape::Cube, Op::Add, hit, normal, 7);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    // position = hit + unit_normal * 0.5 -> {1, 2.5, 3}
    check_float3_approx(node->position, simd_float3{1.0f, 2.5f, 3.0f});
    CHECK(node->snapped == true);
    check_float3_approx(node->snap_point, hit);
    check_float3_approx(node->snap_normal, normal);
    CHECK(node->snap_parent == 7);
}

TEST_CASE("SceneDocument::spawn_snapped: non-axis normal offsets by 0.5 along the normal") {
    SceneDocument doc;

    const simd_float3 hit = {4.0f, 5.0f, 6.0f};
    const simd_float3 normal = simd_normalize(simd_float3{1.0f, 1.0f, 0.0f}); // (~0.7071, ~0.7071, 0)
    const int32_t id = doc.spawn_snapped(Shape::Sphere, Op::Subtract, hit, normal, 3);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    const simd_float3 expected_position = hit + normal * 0.5f;
    check_float3_approx(node->position, expected_position);
    CHECK(node->snapped == true);
    check_float3_approx(node->snap_point, hit);
    check_float3_approx(node->snap_normal, normal);
    CHECK(node->snap_parent == 3);
}

// --- spawn_unsnapped ---------------------------------------------------------

TEST_CASE("SceneDocument::spawn_unsnapped: position passed through unmodified, not snapped") {
    SceneDocument doc;

    const simd_float3 position = {-2.0f, 4.5f, 8.0f};
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, position);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    check_float3_approx(node->position, position);
    CHECK(node->snapped == false);
    CHECK(node->snap_parent == kInvalidNode);
}

// --- remove_node -------------------------------------------------------------

TEST_CASE("SceneDocument::remove_node: removes exactly the target node, others keep ids/order") {
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t b = doc.spawn_unsnapped(Shape::Sphere, Op::Add, {1.0f, 0.0f, 0.0f});
    const int32_t c = doc.spawn_unsnapped(Shape::Cube, Op::Subtract, {2.0f, 0.0f, 0.0f});

    doc.remove_node(b);

    REQUIRE(doc.nodes().size() == 2);
    CHECK(doc.nodes()[0].id == a);
    CHECK(doc.nodes()[1].id == c);
    CHECK(doc.find(b) == nullptr);
    // survivors are otherwise untouched
    CHECK(doc.find(a)->name == "Cube 1");
    CHECK(doc.find(c)->name == "Cube 2");
    CHECK(doc.find(c)->op == Op::Subtract);
}

TEST_CASE("SceneDocument::remove_node: removing an unknown id is a no-op") {
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t b = doc.spawn_unsnapped(Shape::Sphere, Op::Add, {1.0f, 0.0f, 0.0f});

    doc.remove_node(9999); // never issued by this document

    REQUIRE(doc.nodes().size() == 2);
    CHECK(doc.nodes()[0].id == a);
    CHECK(doc.nodes()[1].id == b);
}

TEST_CASE("SceneDocument::remove_node: survivors snapped onto the removed node have their "
          "snap state cleared (snapped=false, snap_parent=kInvalidNode), so drag falls back "
          "to camera-facing rather than referencing a dead id; an unrelated snapped pair is "
          "untouched") {
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const simd_float3 normal = {1.0f, 0.0f, 0.0f};
    const int32_t b = doc.spawn_snapped(Shape::Sphere, Op::Add, {0.5f, 0.0f, 0.0f}, normal, a);
    REQUIRE(doc.find(b)->snapped == true);
    REQUIRE(doc.find(b)->snap_parent == a);

    // Unrelated snapped pair (C onto D) that must survive A's removal untouched.
    const int32_t d = doc.spawn_unsnapped(Shape::Cube, Op::Add, {5.0f, 0.0f, 0.0f});
    const int32_t c = doc.spawn_snapped(Shape::Sphere, Op::Add, {5.5f, 0.0f, 0.0f}, normal, d);
    REQUIRE(doc.find(c)->snapped == true);
    REQUIRE(doc.find(c)->snap_parent == d);

    doc.remove_node(a);

    REQUIRE(doc.find(b) != nullptr);
    CHECK(doc.find(b)->snapped == false);
    CHECK(doc.find(b)->snap_parent == kInvalidNode);

    // Unrelated pair (C/D) untouched by A's removal.
    REQUIRE(doc.find(c) != nullptr);
    CHECK(doc.find(c)->snapped == true);
    CHECK(doc.find(c)->snap_parent == d);
}

TEST_CASE("SceneDocument::remove_node: per-shape name counters continue after removal, never reused") {
    SceneDocument doc;

    const int32_t cube1 = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t cube2 = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    REQUIRE(doc.find(cube2)->name == "Cube 2");

    doc.remove_node(cube2);
    const int32_t cube3 = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});

    // The freed "Cube 2" name/number is never reissued — the counter tracks
    // total spawns of that shape, not currently-live nodes.
    CHECK(doc.find(cube3)->name == "Cube 3");
    CHECK(doc.find(cube1)->name == "Cube 1");
    CHECK(doc.nodes().size() == 2); // cube1, cube3 — cube2 stays gone
}

// --- Editor::deleteSelectedNode -----------------------------------------------
//
// Editor-level tests, in this file per the delete-op brief (mirrors the
// "Editor integration" sections already living alongside SceneDocument-
// adjacent tests in picking_tests.cpp). No Metal objects are constructed —
// Editor::create() + the public query surface only, matching drag_tests.cpp's
// headless pattern.

TEST_CASE("Editor: deleteSelectedNode with no selection is a safe no-op") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    REQUIRE(editor->selectedNode() == kInvalidNode);

    editor->deleteSelectedNode(); // must not crash

    CHECK(editor->selectedNode() == kInvalidNode);
}

TEST_CASE("Editor: deleteSelectedNode removes the selected node and clears selection") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);
    REQUIRE(editor->selectedNode() == spawned.node_id);

    editor->deleteSelectedNode();

    CHECK(editor->selectedNode() == kInvalidNode);
    // The node is truly gone from the document, not just deselected: an
    // unknown-id query returns the documented {0,0,0} default, and a pick at
    // the same view point (now an empty scene) misses.
    check_float3_approx(to_simd(editor->nodePosition(spawned.node_id)), simd_float3{0.0f, 0.0f, 0.0f});
    CHECK(editor->pick(400.0f, 250.0f).node_id == kInvalidNode);
}

TEST_CASE("Editor: deleteSelectedNode only removes the selected node; other nodes are unaffected") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult a = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(a.snapped == false);
    // Far corner: misses A, so B spawns unsnapped and independently selected.
    const SpawnResult b = editor->spawn(Shape::Sphere, Op::Subtract, 100.0f, 100.0f);
    REQUIRE(b.snapped == false);
    REQUIRE(editor->selectedNode() == b.node_id);

    const simd_float3 a_before = to_simd(editor->nodePosition(a.node_id));

    editor->deleteSelectedNode(); // deletes B, the current selection

    CHECK(editor->selectedNode() == kInvalidNode);
    check_float3_approx(to_simd(editor->nodePosition(a.node_id)), a_before); // A untouched
    check_float3_approx(to_simd(editor->nodePosition(b.node_id)), simd_float3{0.0f, 0.0f, 0.0f}); // B gone
}

// Selection/gesture-clearing note (see task brief): Editor::deleteSelectedNode
// resets an active drag/scale gesture when it references the deleted node,
// mirroring setMode's mid-gesture abort. That reset is not independently
// observable through the headless public surface: updateDrag/updateScale
// already no-op whenever the live selection doesn't match the gesture's
// captured node id (the M6 defense-in-depth guard), deleteSelectedNode always
// clears the selection, and SceneDocument ids are never reused — so no future
// selection can ever match a stale gesture's captured id again, reset or not.
// The test below only pins the required safety property (no crash, no
// corruption of an unrelated node) across a delete-mid-gesture sequence.
TEST_CASE("Editor: deleteSelectedNode during an active drag leaves the editor in a safe, "
          "gesture-free state") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    editor->beginDrag(400.0f, 250.0f);
    editor->deleteSelectedNode();
    CHECK(editor->selectedNode() == kInvalidNode);

    // deleteSelectedNode already closed the gesture above (drag.active reset
    // to false, per the mid-gesture abort this test targets), so these two
    // calls exercise updateDrag/endDrag's plain "not active" no-op guards --
    // pinning that a post-delete call into either is safe, not that a
    // gesture is somehow still open.
    editor->updateDrag(500.0f, 250.0f); // must not crash
    editor->endDrag();                  // must not crash

    // A fresh node created after the delete is unaffected by the stale
    // gesture (nothing moved it: no matching beginDrag was issued for it).
    const SpawnResult respawned = editor->spawn(Shape::Cube, Op::Add, 200.0f, 200.0f);
    const simd_float3 before = to_simd(editor->nodePosition(respawned.node_id));
    editor->updateDrag(300.0f, 300.0f); // still no active drag for this node: no-op
    check_float3_approx(to_simd(editor->nodePosition(respawned.node_id)), before);
}
