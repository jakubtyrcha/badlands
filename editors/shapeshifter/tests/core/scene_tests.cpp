#include <doctest.h>

#include <cmath>
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

TEST_CASE("SceneDocument::spawn_snapped: the node is CENTRED on the surface, not resting on it") {
    SceneDocument doc;

    const simd_float3 hit = {1.0f, 2.0f, 3.0f};
    const simd_float3 normal = {0.0f, 1.0f, 0.0f};
    const int32_t id = doc.spawn_snapped(Shape::Cube, Op::Add, hit, normal, 7);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    // position == hit. This used to be hit + normal * 0.5, which left the
    // shape sitting on top of its parent instead of half-embedded in it, and
    // put the node's centre permanently half a unit off its own snap point.
    check_float3_approx(node->local_position, hit);
    check_float3_approx(node->local_position, node->contact.point); // the coincidence the gizmos rely on
    CHECK(node->contact.valid == true);
    check_float3_approx(node->contact.point, hit);
    check_float3_approx(node->contact.normal, normal);
    CHECK(node->contact.surface == 7);
}

TEST_CASE("SceneDocument::spawn_snapped: an oblique normal does not displace the node either") {
    // The normal still defines the snap FRAME (and so the placement gizmo's
    // basis); it just no longer displaces the node along itself.
    SceneDocument doc;

    const simd_float3 hit = {4.0f, 5.0f, 6.0f};
    const simd_float3 normal = simd_normalize(simd_float3{1.0f, 1.0f, 0.0f}); // (~0.7071, ~0.7071, 0)
    const int32_t id = doc.spawn_snapped(Shape::Sphere, Op::Subtract, hit, normal, 3);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    check_float3_approx(node->local_position, hit);
    CHECK(node->contact.valid == true);
    check_float3_approx(node->contact.point, hit);
    check_float3_approx(node->contact.normal, normal);
    CHECK(node->contact.surface == 3);
}

// --- spawn_unsnapped ---------------------------------------------------------

TEST_CASE("SceneDocument::spawn_unsnapped: position passed through unmodified, not snapped") {
    SceneDocument doc;

    const simd_float3 position = {-2.0f, 4.5f, 8.0f};
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, position);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    check_float3_approx(node->local_position, position);
    CHECK(node->contact.valid == false);
    CHECK(node->contact.surface == kInvalidNode);
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
    REQUIRE(doc.find(b)->contact.valid == true);
    REQUIRE(doc.find(b)->contact.surface == a);

    // Unrelated snapped pair (C onto D) that must survive A's removal untouched.
    const int32_t d = doc.spawn_unsnapped(Shape::Cube, Op::Add, {5.0f, 0.0f, 0.0f});
    const int32_t c = doc.spawn_snapped(Shape::Sphere, Op::Add, {5.5f, 0.0f, 0.0f}, normal, d);
    REQUIRE(doc.find(c)->contact.valid == true);
    REQUIRE(doc.find(c)->contact.surface == d);

    doc.remove_node(a);

    REQUIRE(doc.find(b) != nullptr);
    CHECK(doc.find(b)->contact.valid == false);
    CHECK(doc.find(b)->contact.surface == kInvalidNode);

    // Unrelated pair (C/D) untouched by A's removal.
    REQUIRE(doc.find(c) != nullptr);
    CHECK(doc.find(c)->contact.valid == true);
    CHECK(doc.find(c)->contact.surface == d);
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

// --- the shape parameter ------------------------------------------------------

TEST_CASE("SceneDocument: every shape auto-names from its own counter") {
    // The counters became a table when the shape list went from two to eight;
    // this pins that each shape still counts independently, and that the names
    // match the enum's order rather than being off by one.
    SceneDocument doc;
    struct Case { Shape shape; const char* expected; };
    const Case cases[] = {
        {Shape::Cube, "Cube 1"},         {Shape::Sphere, "Sphere 1"},
        {Shape::Cone, "Cone 1"},         {Shape::Capsule, "Capsule 1"},
        {Shape::Octahedron, "Octahedron 1"}, {Shape::Pyramid, "Pyramid 1"},
        {Shape::Prism, "Prism 1"},       {Shape::Vesica, "Vesica 1"},
        {Shape::Cone, "Cone 2"},         {Shape::Cube, "Cube 2"},
    };
    for (const Case& c : cases) {
        const int32_t id = doc.spawn_unsnapped(c.shape, Op::Add, simd_float3{0, 0, 0});
        const Node* node = doc.find(id);
        REQUIRE(node != nullptr);
        CHECK(node->name == std::string(c.expected));
    }
}

TEST_CASE("a freshly spawned shape gets its spec's default parameter") {
    // Defaults are chosen so a shape looks like its own name on arrival: a cone
    // is sharp, a capsule is fully round, a prism is a hexagon.
    SceneDocument doc;
    struct Case { Shape shape; float expected; };
    const Case cases[] = {
        {Shape::Cone, 0.0f}, {Shape::Capsule, 1.0f}, {Shape::Pyramid, 0.0f},
        {Shape::Prism, 6.0f}, {Shape::Cube, 0.0f}, {Shape::Vesica, 0.0f},
    };
    for (const Case& c : cases) {
        const int32_t id = doc.spawn_unsnapped(c.shape, Op::Add, simd_float3{0, 0, 0});
        const Node* node = doc.find(id);
        REQUIRE(node != nullptr);
        CHECK(node->shape_param == doctest::Approx(c.expected));
    }
}

TEST_CASE("snap_shape_param clamps first, then snaps to the spec's step") {
    const ShapeParamSpec cone = shape_param_spec(Shape::Cone);
    REQUIRE(cone.has_param);
    CHECK(snap_shape_param(cone, 0.47f) == doctest::Approx(0.45f));
    CHECK(snap_shape_param(cone, 0.475f) == doctest::Approx(0.5f));   // ties round up
    CHECK(snap_shape_param(cone, -3.0f) == doctest::Approx(0.0f));    // clamped to the low end
    CHECK(snap_shape_param(cone, 99.0f) == doctest::Approx(1.0f));    // and to the high end
    // NaN cannot be allowed through: it would reach params.x, and one NaN in
    // the fold takes the whole scene's surface with it.
    CHECK(snap_shape_param(cone, std::nanf("")) == doctest::Approx(0.0f));

    const ShapeParamSpec prism = shape_param_spec(Shape::Prism);
    REQUIRE(prism.has_param);
    CHECK(prism.integral);
    CHECK(snap_shape_param(prism, 6.4f) == doctest::Approx(6.0f));
    CHECK(snap_shape_param(prism, 6.6f) == doctest::Approx(7.0f));
    CHECK(snap_shape_param(prism, 0.0f) == doctest::Approx(3.0f));    // below the minimum
    CHECK(snap_shape_param(prism, 100.0f) == doctest::Approx(12.0f));

    // The sphere is the ONLY shape without a parameter -- an ellipsoid is
    // already the roundest thing its box allows, so there is nothing left to
    // vary. It snaps everything to zero, so nothing downstream has to ask
    // whether the value means anything.
    const ShapeParamSpec sphere = shape_param_spec(Shape::Sphere);
    CHECK_FALSE(sphere.has_param);
    CHECK(snap_shape_param(sphere, 0.7f) == doctest::Approx(0.0f));
    for (int32_t i = 0; i < kShapeCount; ++i) {
        const Shape shape = static_cast<Shape>(i);
        INFO("shape id: " << i);
        CHECK(shape_param_spec(shape).has_param == (shape != Shape::Sphere));
    }
}

TEST_CASE("Editor::setNodeShapeParam is the only way in, and it snaps") {
    Editor* editor = Editor::create();
    REQUIRE(editor != nullptr);
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult cone = editor->spawn(Shape::Cone, Op::Add, 400.0f, 250.0f);
    REQUIRE(cone.node_id != kInvalidNode);
    CHECK(editor->nodeShape(cone.node_id) == Shape::Cone);
    CHECK(editor->nodeShapeParam(cone.node_id) == doctest::Approx(0.0f));

    editor->setNodeShapeParam(cone.node_id, 0.63f);
    CHECK(editor->nodeShapeParam(cone.node_id) == doctest::Approx(0.65f));
    editor->setNodeShapeParam(cone.node_id, 5.0f);
    CHECK(editor->nodeShapeParam(cone.node_id) == doctest::Approx(1.0f));

    const ShapeParamSpec spec = editor->nodeShapeParamSpec(cone.node_id);
    CHECK(spec.has_param);
    CHECK(spec.min_value == doctest::Approx(0.0f));
    CHECK(spec.max_value == doctest::Approx(1.0f));
    CHECK(spec.step == doctest::Approx(0.05f));
    CHECK_FALSE(spec.integral);

    // An unknown id reports no parameter rather than a default-looking one:
    // "there is no dial here" is the only question the caller is asking.
    CHECK_FALSE(editor->nodeShapeParamSpec(kInvalidNode).has_param);
    editor->setNodeShapeParam(kInvalidNode, 0.5f); // no-op, must not crash
}

TEST_CASE("Editor: every shape survives the whole app-facing round trip") {
    // The path the app layer actually walks, per shape: spawn -> select ->
    // turn the dial -> pick it again. Each step is covered in isolation
    // elsewhere; what this adds is that they compose, which is the thing a
    // per-shape bug would break without failing any single-purpose test.
    struct Case { const char* label; Shape shape; float dial; };
    const Case cases[] = {
        {"cube", Shape::Cube, 0.0f},              {"sphere", Shape::Sphere, 0.0f},
        {"cone", Shape::Cone, 0.55f},             {"capsule", Shape::Capsule, 0.3f},
        {"octahedron", Shape::Octahedron, 0.0f},  {"pyramid", Shape::Pyramid, 0.7f},
        {"prism", Shape::Prism, 9.0f},            {"vesica", Shape::Vesica, 0.0f},
    };

    for (const Case& c : cases) {
        INFO("case: " << std::string(c.label));
        Editor* editor = Editor::create();
        REQUIRE(editor != nullptr);
        editor->setViewportSize(800.0f, 500.0f, 2.0f);

        // Spawn at the viewport centre: nothing is there yet, so this is the
        // unsnapped path, landing the node on the centre ray.
        const SpawnResult spawned = editor->spawn(c.shape, Op::Add, 400.0f, 250.0f);
        REQUIRE(spawned.node_id != kInvalidNode);
        CHECK_FALSE(spawned.snapped);
        CHECK(editor->selectedNode() == spawned.node_id);
        CHECK(editor->nodeShape(spawned.node_id) == c.shape);

        editor->setNodeShapeParam(spawned.node_id, c.dial);
        const ShapeParamSpec spec = editor->nodeShapeParamSpec(spawned.node_id);
        if (spec.has_param) {
            CHECK(editor->nodeShapeParam(spawned.node_id)
                  == doctest::Approx(snap_shape_param(spec, c.dial)));
        }

        // And it can be picked back off the screen it was placed on -- the
        // round trip that fails if a shape's SDF and its spawn placement
        // disagree about where the surface is.
        const PickResult picked = editor->pick(400.0f, 250.0f);
        CHECK(picked.node_id == spawned.node_id);

        // A second shape snapped onto the first: spawn_snapped centres the new
        // node on the surface point picking just reported, so this exercises
        // the pick -> spawn handoff for every shape as the placement target.
        const SpawnResult attached = editor->spawn(Shape::Sphere, Op::Add, 400.0f, 250.0f);
        REQUIRE(attached.node_id != kInvalidNode);
        CHECK(attached.snapped);
    }
}

// --- placement(): the single placement resolver -----------------------------
//
// Pins what placement() answers while it still reads the node's world-space
// fields directly. These expectations must survive the storage becoming
// parent-local: a world-rooted node's placement is the same either way, which
// is exactly the claim the rework rests on.

TEST_CASE("placement reports a spawned node's world frame") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});

    const NodePlacement p = doc.placement(id);
    check_float3_approx(p.frame.position, simd_float3{1, 2, 3});
    CHECK(p.frame.uniform_scale == doctest::Approx(1.0f));
    CHECK(simd_length(p.frame.rotation.vector) == doctest::Approx(1.0f));
    // half_extents is scale * 0.5, the same quantity pack_scene writes.
    check_float3_approx(p.half_extents, simd_float3{0.5f, 0.5f, 0.5f});
    CHECK_FALSE(p.contact.has_value());
    CHECK(p.binding_resolved);
}

TEST_CASE("placement carries a node's rotation and non-uniform scale") {
    SceneDocument doc;
    Node n;
    n.id = 7;
    n.shape = Shape::Cube;
    n.local_position = simd_float3{0, 1, 0};
    n.local_rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0});
    n.scale = simd_float3{2, 4, 8};
    doc.add(n);

    const NodePlacement p = doc.placement(7);
    check_float3_approx(p.frame.position, simd_float3{0, 1, 0});
    // Per-node non-uniform scale is untouched: it lives in half_extents and is
    // applied in the node's own local space, so it never becomes shear.
    check_float3_approx(p.half_extents, simd_float3{1, 2, 4});
    check_float3_approx(simd_act(p.frame.rotation, simd_float3{1, 0, 0}), simd_float3{0, 0, -1});
}

// simd_abs, matching append_node_wireframe and sdf_safe_half_extents: the
// evaluator measures against abs(half_extents), so a negative component
// mirrors the solid rather than inverting the box.
TEST_CASE("placement reports absolute half-extents for a negative scale") {
    SceneDocument doc;
    Node n;
    n.id = 1;
    n.scale = simd_float3{-2, 1, 1};
    doc.add(n);

    check_float3_approx(doc.placement(1).half_extents, simd_float3{1, 0.5f, 0.5f});
}

TEST_CASE("placement reports a snapped node's contact in world space") {
    SceneDocument doc;
    const int32_t base = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = doc.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                             simd_float3{0, 1, 0}, base);

    const NodePlacement p = doc.placement(detail);
    REQUIRE(p.contact.has_value());
    check_float3_approx(p.contact->point, simd_float3{0, 0.5f, 0});
    check_float3_approx(p.contact->normal, simd_float3{0, 1, 0});
}

// The tether's whole subject: dragging a detail moves it, and deliberately
// leaves its contact on the surface it was placed on.
TEST_CASE("placement's contact does not follow the node's position") {
    SceneDocument doc;
    const int32_t base = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = doc.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                             simd_float3{0, 1, 0}, base);
    doc.find(detail)->local_position = simd_float3{0, 3, 0};

    const NodePlacement p = doc.placement(detail);
    check_float3_approx(p.frame.position, simd_float3{0, 3, 0});
    REQUIRE(p.contact.has_value());
    check_float3_approx(p.contact->point, simd_float3{0, 0.5f, 0});
}

TEST_CASE("placement of an unknown id is the default") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{5, 5, 5});

    const NodePlacement p = doc.placement(kInvalidNode);
    check_float3_approx(p.frame.position, simd_float3{0, 0, 0});
    check_float3_approx(p.half_extents, simd_float3{0, 0, 0});
    CHECK_FALSE(p.contact.has_value());
}
