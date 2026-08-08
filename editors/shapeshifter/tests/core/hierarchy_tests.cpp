#include <doctest.h>

#include <cmath>

#include <shapeshifter/ShapeshifterCore.h>

#include "scene.h"
#include "sdf.h"

using namespace sq;

// The transform hierarchy, exercised directly.
//
// Nothing in the editor BUILDS one of these yet -- spawn_snapped deliberately
// leaves every node world-rooted, which is what pack_baseline_tests proves. So
// these are the only place the propagation rules run at all, and they are
// written against the document API rather than through the Editor for exactly
// that reason.

namespace {

void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Parents `child` to `parent` by writing the field directly. attach() (which
// preserves world pose and rejects cycles) is a later concern; these cases are
// about what the RESOLVER does once a parent exists.
void set_parent(SceneDocument& doc, int32_t child, int32_t parent) {
    Node* n = doc.find(child);
    REQUIRE(n != nullptr);
    n->parent.kind = ParentRef::Kind::Node;
    n->parent.node = parent;
}

} // namespace

// --- the propagation rule ---------------------------------------------------

TEST_CASE("a Group's uniform scale scales its child's offset and its box") {
    SceneDocument doc;
    const int32_t group = doc.add_group();
    doc.set_node_scale(group, simd_float3{2, 2, 2});

    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, child, group);

    const NodePlacement p = doc.placement(child);
    check_float3_approx(p.frame.position, simd_float3{2, 0, 0}); // the OFFSET scaled, not just the box
    CHECK(p.frame.uniform_scale == doctest::Approx(2.0f));
    check_float3_approx(p.half_extents, simd_float3{1, 1, 1});   // 1 * 0.5 * 2
}

// The rule that keeps stretching a skull from smearing the horn on it, and
// Maya's segmentScaleCompensate default stated as a test.
TEST_CASE("a Shape parent's non-uniform scale reaches its child in no way at all") {
    SceneDocument doc;
    const int32_t skull = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.set_node_scale(skull, simd_float3{5, 1, 1}); // stretched long

    const int32_t horn = doc.spawn_unsnapped(Shape::Cone, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, horn, skull);

    const NodePlacement p = doc.placement(horn);
    check_float3_approx(p.frame.position, simd_float3{1, 0, 0}); // NOT {5,0,0}
    CHECK(p.frame.uniform_scale == doctest::Approx(1.0f));
    check_float3_approx(p.half_extents, simd_float3{0.5f, 0.5f, 0.5f});
}

// Right-handed, Y up: +90 degrees about +Y sends +X to -Z.
TEST_CASE("a parent's rotation swings its child around it") {
    SceneDocument doc;
    const int32_t parent = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.find(parent)->local_rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0});

    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, child, parent);

    const NodePlacement p = doc.placement(child);
    check_float3_approx(p.frame.position, simd_float3{0, 0, -1});
    // And the child's own axes turn with it.
    check_float3_approx(simd_act(p.frame.rotation, simd_float3{1, 0, 0}), simd_float3{0, 0, -1});
}

TEST_CASE("Group scale composes multiplicatively through a chain") {
    SceneDocument doc;
    const int32_t outer = doc.add_group();
    doc.set_node_scale(outer, simd_float3{2, 2, 2});
    const int32_t inner = doc.add_group();
    doc.set_node_scale(inner, simd_float3{3, 3, 3});
    set_parent(doc, inner, outer);

    const int32_t leaf = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, leaf, inner);

    const NodePlacement p = doc.placement(leaf);
    CHECK(p.frame.uniform_scale == doctest::Approx(6.0f));
    check_float3_approx(p.frame.position, simd_float3{6, 0, 0});
    check_float3_approx(p.half_extents, simd_float3{3, 3, 3});
}

// A Shape in the middle of a chain passes a Group's scale through without
// adding its own -- the two rules above, meeting.
TEST_CASE("a Shape in a chain passes an ancestor Group's scale through unchanged") {
    SceneDocument doc;
    const int32_t group = doc.add_group();
    doc.set_node_scale(group, simd_float3{2, 2, 2});

    const int32_t middle = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.set_node_scale(middle, simd_float3{9, 9, 9}); // its own box, and nobody else's business
    set_parent(doc, middle, group);

    const int32_t leaf = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, leaf, middle);

    const NodePlacement p = doc.placement(leaf);
    CHECK(p.frame.uniform_scale == doctest::Approx(2.0f)); // the Group's, not 18
    check_float3_approx(p.frame.position, simd_float3{2, 0, 0});
}

// --- Group invariants -------------------------------------------------------

TEST_CASE("a Group's scale is forced uniform") {
    SceneDocument doc;
    const int32_t group = doc.add_group();
    doc.set_node_scale(group, simd_float3{2, 7, 9}); // only x is meaningful

    const Node* n = doc.find(group);
    REQUIRE(n != nullptr);
    check_float3_approx(n->scale, simd_float3{2, 2, 2});
}

TEST_CASE("a Shape's scale is written through as given") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.set_node_scale(id, simd_float3{2, 7, 9});

    check_float3_approx(doc.find(id)->scale, simd_float3{2, 7, 9});
}

// A Group carries a frame, not geometry. Packing it with a zero extent would
// not be harmless: sdf_eval_node floors half-extents at SDF_MIN_HALF_EXTENT, so
// it would render as a speck.
TEST_CASE("a Group contributes no SDF node and no extent") {
    SceneDocument doc;
    const int32_t group = doc.add_group();
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});

    CHECK(pack_scene(doc).size() == 1);
    check_float3_approx(doc.placement(group).half_extents, simd_float3{0, 0, 0});
}

TEST_CASE("a Group is named off its own counter") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t g1 = doc.add_group();
    const int32_t g2 = doc.add_group();

    CHECK(doc.find(g1)->name == "Group 1");
    CHECK(doc.find(g2)->name == "Group 2");
}

// --- the contact rides the PARENT, not the node -----------------------------

TEST_CASE("the contact is expressed in the parent's frame") {
    SceneDocument doc;
    const int32_t group = doc.add_group();
    doc.set_node_scale(group, simd_float3{2, 2, 2});
    doc.find(group)->local_position = simd_float3{10, 0, 0};

    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    set_parent(doc, child, group);
    Node* n = doc.find(child);
    n->contact.valid = true;
    n->contact.point = simd_float3{1, 0, 0};
    n->contact.normal = simd_float3{0, 1, 0};

    const NodePlacement p = doc.placement(child);
    REQUIRE(p.contact.has_value());
    // Scaled and translated by the PARENT's frame: 10 + 2*1.
    check_float3_approx(p.contact->point, simd_float3{12, 0, 0});
    // The normal is rotated only, so it stays unit -- the gizmo builds a
    // tangent basis from it and a scaled normal would skew the whole frame.
    check_float3_approx(p.contact->normal, simd_float3{0, 1, 0});
    CHECK(simd_length(p.contact->normal) == doctest::Approx(1.0f));
}

TEST_CASE("a parent's rotation rotates the contact normal") {
    SceneDocument doc;
    const int32_t parent = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.find(parent)->local_rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 0, 1});

    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    set_parent(doc, child, parent);
    doc.find(child)->contact.valid = true;
    doc.find(child)->contact.normal = simd_float3{0, 1, 0};

    const NodePlacement p = doc.placement(child);
    REQUIRE(p.contact.has_value());
    // +90 about +Z sends +Y to -X.
    check_float3_approx(p.contact->normal, simd_float3{-1, 0, 0});
}

// --- degradation ------------------------------------------------------------

// Only a direct field write can build this: attach() rejects cycles. The guard
// exists so a corrupt document cannot hang the render loop or blow the stack.
TEST_CASE("a hand-built cycle terminates and places nothing") {
    SceneDocument doc;
    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    const int32_t b = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 1, 0});
    set_parent(doc, a, b);
    set_parent(doc, b, a);

    const NodePlacement p = doc.placement(a);
    check_float3_approx(p.frame.position, simd_float3{0, 0, 0});
    check_float3_approx(p.half_extents, simd_float3{0, 0, 0});
    CHECK_FALSE(p.contact.has_value());
}

TEST_CASE("a self-parented node terminates and places nothing") {
    SceneDocument doc;
    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    set_parent(doc, a, a);

    check_float3_approx(doc.placement(a).frame.position, simd_float3{0, 0, 0});
}

TEST_CASE("a dangling parent id falls back to world-rooted") {
    SceneDocument doc;
    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    set_parent(doc, child, 9999); // never existed

    const NodePlacement p = doc.placement(child);
    check_float3_approx(p.frame.position, simd_float3{1, 2, 3});
    CHECK(p.binding_resolved); // a missing NODE is not an unresolved BINDING
}

// No FrameProvider is wired up yet, so a named attachment cannot resolve. The
// node stays usable -- world-rooted -- and says so, rather than sitting at the
// origin with no explanation.
TEST_CASE("an attachment-named parent is unresolved and falls back to world-rooted") {
    SceneDocument doc;
    const int32_t child = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    Node* n = doc.find(child);
    n->parent.kind = ParentRef::Kind::Attachment;
    n->parent.attachment = "hand.R";

    const NodePlacement p = doc.placement(child);
    check_float3_approx(p.frame.position, simd_float3{1, 2, 3});
    CHECK_FALSE(p.binding_resolved);
}

// --- what removal does to the two relations ---------------------------------

TEST_CASE("removing a node invalidates contacts resting on it") {
    SceneDocument doc;
    const int32_t base = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = doc.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                             simd_float3{0, 1, 0}, base);
    REQUIRE(doc.placement(detail).contact.has_value());

    doc.remove_node(base);
    CHECK_FALSE(doc.placement(detail).contact.has_value());
    // The detail itself survives, where it stood.
    check_float3_approx(doc.placement(detail).frame.position, simd_float3{0, 0.5f, 0});
}

// --- the inertness ruling, from the hierarchy's side -------------------------

TEST_CASE("spawn_snapped records a contact but does NOT parent") {
    SceneDocument doc;
    const int32_t base = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = doc.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                             simd_float3{0, 1, 0}, base);

    const Node* n = doc.find(detail);
    REQUIRE(n != nullptr);
    CHECK(n->parent.kind == ParentRef::Kind::World); // inert: nothing propagates
    CHECK(n->contact.valid);
    CHECK(n->contact.surface == base);
}

// The consequence that ruling buys, stated as behaviour rather than as a field
// check: moving the surface leaves the detail where it was.
TEST_CASE("moving a node a detail rests on does not move the detail") {
    SceneDocument doc;
    const int32_t base = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = doc.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                             simd_float3{0, 1, 0}, base);

    doc.find(base)->local_position = simd_float3{100, 0, 0};

    check_float3_approx(doc.placement(detail).frame.position, simd_float3{0, 0.5f, 0});
}
