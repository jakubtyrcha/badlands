#include <doctest.h>

#include <cmath>
#include <vector>

#include <shapeshifter/ShapeshifterCore.h>

#include "scene.h"
#include "sdf.h"
#include "sdf_scene.h"

using namespace sq;

// THE INERTNESS GATE.
//
// pack_scene's OUTPUT BYTES, pinned for documents built through the real spawn
// paths. The document model is being rebuilt underneath this -- world-space
// fields become parent-local, four snap_* fields become a Contact, a parent
// chain and node kinds arrive -- and the whole claim of that rework is that it
// changes no rendered pixel. This is the assertion that claim reduces to.
//
// Unusual for a test in this suite: it must be GREEN THE MOMENT IT IS WRITTEN,
// because it describes what already happens. Every literal is hand-derived from
// today's semantics -- half_extents = abs(scale) * 0.5, inv_rotation =
// conjugate (NOT a general inverse, see sdf.cpp), op 0 = Add / 1 = Subtract,
// pos_shape.w = the Shape enum's own value.
//
// If it ever fails during the rework, the inertness ruling has been broken.
// Stop and find out where, rather than adjusting the literals -- adjusting them
// is precisely the failure this file exists to catch.

TEST_CASE("pack_scene output is pinned for a spawned document") {
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    // Centred ON the hit, not resting on it -- spawn_snapped's own contract.
    const int32_t b = doc.spawn_snapped(Shape::Capsule, Op::Subtract,
                                        simd_float3{0, 1, 0}, simd_float3{0, 1, 0}, a);
    REQUIRE(a != kInvalidNode);
    REQUIRE(b != kInvalidNode);

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 2);

    // Node A: Cube (shape id 0), Add (op 0), unit scale, identity rotation.
    // The cube's default shape_param is 0 (sharp corners).
    CHECK(packed[0].pos_shape.x == doctest::Approx(1.0f));
    CHECK(packed[0].pos_shape.y == doctest::Approx(2.0f));
    CHECK(packed[0].pos_shape.z == doctest::Approx(3.0f));
    CHECK(packed[0].pos_shape.w == doctest::Approx(0.0f));
    CHECK(packed[0].half_extents_op.x == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.y == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.z == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.w == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.x == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.y == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.z == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.w == doctest::Approx(1.0f));
    CHECK(packed[0].params.x == doctest::Approx(0.0f));

    // Node B: Capsule (shape id 3), Subtract (op 1), positioned at the hit.
    // The capsule's default shape_param is 1.0 (fully round caps), which is
    // what makes a freshly spawned capsule look like its own name.
    CHECK(packed[1].pos_shape.x == doctest::Approx(0.0f));
    CHECK(packed[1].pos_shape.y == doctest::Approx(1.0f));
    CHECK(packed[1].pos_shape.z == doctest::Approx(0.0f));
    CHECK(packed[1].pos_shape.w == doctest::Approx(3.0f));
    CHECK(packed[1].half_extents_op.x == doctest::Approx(0.5f));
    CHECK(packed[1].half_extents_op.w == doctest::Approx(1.0f));
    CHECK(packed[1].inv_rotation.w == doctest::Approx(1.0f));
    CHECK(packed[1].params.x == doctest::Approx(1.0f));
}

// Node ORDER is semantic: sdf_fold reduces in vector order and a Subtract
// carves everything accumulated before it. Nothing in the rework may reorder
// the node vector, so pin the order itself rather than only the contents.
TEST_CASE("pack_scene preserves spawn order") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.spawn_unsnapped(Shape::Sphere, Op::Subtract, simd_float3{1, 0, 0});
    doc.spawn_unsnapped(Shape::Cone, Op::Add, simd_float3{2, 0, 0});

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 3);

    // Shape ids in spawn order: Cube 0, Sphere 1, Cone 2.
    CHECK(packed[0].pos_shape.w == doctest::Approx(0.0f));
    CHECK(packed[1].pos_shape.w == doctest::Approx(1.0f));
    CHECK(packed[2].pos_shape.w == doctest::Approx(2.0f));
    // And the ops that make the order matter at all.
    CHECK(packed[0].half_extents_op.w == doctest::Approx(0.0f));
    CHECK(packed[1].half_extents_op.w == doctest::Approx(1.0f));
    CHECK(packed[2].half_extents_op.w == doctest::Approx(0.0f));
}

// The conjugate-not-inverse contract, and non-uniform scale reaching
// half_extents. A rotated, non-uniformly scaled node is exactly the case the
// similarity-only rework must leave untouched: the scale is applied in the
// node's OWN local space, before its rotation, so it never composes with
// anything and never becomes shear.
TEST_CASE("pack_scene pins the conjugate and non-uniform half-extents") {
    SceneDocument doc;
    Node n;
    n.id = 1;
    n.shape = Shape::Cube;
    n.op = Op::Add;
    n.local_position = simd_float3{0, 0, 0};
    n.local_rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0}); // +90 degrees about Y
    n.scale = simd_float3{2.0f, 1.0f, 0.5f};
    doc.add(n);

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 1);

    CHECK(packed[0].half_extents_op.x == doctest::Approx(1.0f));
    CHECK(packed[0].half_extents_op.y == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.z == doctest::Approx(0.25f));

    // conjugate(q) negates the imaginary part, so (0, sin(pi/4), 0, cos(pi/4))
    // packs as (0, -sin(pi/4), 0, cos(pi/4)). That identity holds ONLY for a
    // unit quaternion, which is why every producer of Node::rotation
    // renormalizes -- pinning it here is what keeps a future producer honest.
    CHECK(packed[0].inv_rotation.x == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.y == doctest::Approx(-std::sin(float(M_PI_4))));
    CHECK(packed[0].inv_rotation.z == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.w == doctest::Approx(std::cos(float(M_PI_4))));
}

// An empty document packs to nothing, and the reserve() in pack_scene must not
// leave a stale element behind when a populated vector is reused.
TEST_CASE("pack_scene clears its output before filling it") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});

    std::vector<SdfNode> packed;
    pack_scene(doc, packed);
    REQUIRE(packed.size() == 1);

    const SceneDocument empty;
    pack_scene(empty, packed);
    CHECK(packed.empty());
}
