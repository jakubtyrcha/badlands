#include <doctest.h>

#include <utility>
#include <vector>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "sdf.h"
#include "sdf_scene.h"
#include "scene.h"

using namespace sq;

// This file pins two pieces introduced by task R0:
//  - pack_scene()'s exact SdfNode packing (including the >128-node cap).
//  - sdf_ray_for_pixel()'s agreement with the already-pinned
//    Camera::ray_through_view_point() (existing camera_tests.cpp literals
//    are the reference; agreement itself is the assertion, per the brief —
//    no new numpy derivation needed for this half).
//
// sdf_tests.cpp (sd_box/sd_ellipsoid/evaluate_scene_sdf/sample_scene) is
// intentionally untouched by this task — it is the refactor guard.

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components — matches sdf_tests.cpp's check_float3_approx.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected, double eps = 1e-5) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(eps));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(eps));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(eps));
}

Camera make_camera(float aspect) {
    // Same pinned setup as camera_tests.cpp's make_camera(): reusing these
    // literals directly (not re-deriving with numpy) is the point — this
    // test's assertion is agreement between the two ray formulas, not a
    // fresh pin of either one.
    Camera camera;
    camera.eye = {4.0f, 3.0f, 6.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f;
    camera.aspect = aspect;
    return camera;
}

} // namespace

// --- SdfNode layout -----------------------------------------------------------

TEST_CASE("SdfNode is 32 bytes (compile-time; static_assert in sdf_scene.h covers both "
          "the C++ and MSL sides -- this test just confirms the C++ side is actually "
          "exercised by this binary)") {
    CHECK(sizeof(SdfNode) == 32u);
}

// --- pack_scene: exact packing ---------------------------------------------

TEST_CASE("pack_scene: 3-node document (cube add / sphere subtract / cube add, "
          "nonuniform scales) packs to exact SdfNode values in document order") {
    // Packing is a direct, exact transcription (position copied verbatim,
    // half_extents = scale*0.5, shape/op mapped to the brief's fixed 0/1
    // encoding) -- no nontrivial formula here, so hand-computed halves
    // (chosen to divide evenly) are pinned directly rather than run through
    // numpy, matching how sdf_tests.cpp already treats purely-mechanical
    // arithmetic (e.g. its AABB test states "half=(1,1,1)" plainly).
    SceneDocument doc;

    Node n1; // cube, add
    n1.id = 1;
    n1.shape = Shape::Cube;
    n1.op = Op::Add;
    n1.position = {1.0f, 2.0f, 3.0f};
    n1.scale = {2.0f, 4.0f, 6.0f}; // half = (1, 2, 3)
    doc.add(n1);

    Node n2; // sphere, subtract
    n2.id = 2;
    n2.shape = Shape::Sphere;
    n2.op = Op::Subtract;
    n2.position = {-1.0f, 0.5f, 2.0f};
    n2.scale = {1.0f, 3.0f, 5.0f}; // half = (0.5, 1.5, 2.5)
    doc.add(n2);

    Node n3; // cube, add
    n3.id = 3;
    n3.shape = Shape::Cube;
    n3.op = Op::Add;
    n3.position = {0.0f, 0.0f, 0.0f};
    n3.scale = {3.0f, 3.0f, 3.0f}; // half = (1.5, 1.5, 1.5)
    doc.add(n3);

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 3u);

    SUBCASE("node 0: cube add, pos (1,2,3), half (1,2,3)") {
        check_float3_approx(packed[0].pos_shape.xyz, simd_float3{1.0f, 2.0f, 3.0f});
        CHECK(packed[0].pos_shape.w == doctest::Approx(0.0f)); // shape: cube
        check_float3_approx(packed[0].half_extents_op.xyz, simd_float3{1.0f, 2.0f, 3.0f});
        CHECK(packed[0].half_extents_op.w == doctest::Approx(0.0f)); // op: add
    }
    SUBCASE("node 1: sphere subtract, pos (-1,0.5,2), half (0.5,1.5,2.5)") {
        check_float3_approx(packed[1].pos_shape.xyz, simd_float3{-1.0f, 0.5f, 2.0f});
        CHECK(packed[1].pos_shape.w == doctest::Approx(1.0f)); // shape: sphere
        check_float3_approx(packed[1].half_extents_op.xyz, simd_float3{0.5f, 1.5f, 2.5f});
        CHECK(packed[1].half_extents_op.w == doctest::Approx(1.0f)); // op: subtract
    }
    SUBCASE("node 2: cube add, pos (0,0,0), half (1.5,1.5,1.5)") {
        check_float3_approx(packed[2].pos_shape.xyz, simd_float3{0.0f, 0.0f, 0.0f});
        CHECK(packed[2].pos_shape.w == doctest::Approx(0.0f));
        check_float3_approx(packed[2].half_extents_op.xyz, simd_float3{1.5f, 1.5f, 1.5f});
        CHECK(packed[2].half_extents_op.w == doctest::Approx(0.0f));
    }
}

// --- pack_scene: >128-node cap ----------------------------------------------

TEST_CASE("pack_scene: a >128-node document caps at kMaxRaymarchNodes, keeping the "
          "first 128 nodes in document order") {
    CHECK(kMaxRaymarchNodes == 128);

    SceneDocument doc;
    for (int32_t i = 0; i < 130; ++i) {
        Node n;
        n.id = i + 1;
        n.shape = Shape::Cube;
        n.op = Op::Add;
        n.position = {static_cast<float>(i), 0.0f, 0.0f}; // x == spawn order, for identification
        n.scale = {1.0f, 1.0f, 1.0f};
        doc.add(n);
    }
    REQUIRE(doc.nodes().size() == 130u);

    const std::vector<SdfNode> packed = pack_scene(doc);

    CHECK(packed.size() == static_cast<size_t>(kMaxRaymarchNodes));
    CHECK(packed.size() == 128u);
    // First 128 kept, in order: node i's x == i, so the kept range is x in [0, 127].
    CHECK(packed.front().pos_shape.x == doctest::Approx(0.0f));
    CHECK(packed.back().pos_shape.x == doctest::Approx(127.0f));
}

// --- pack_scene: out-param overload (R1's per-frame scratch-buffer helper) --

TEST_CASE("pack_scene(doc, out): matches the return-by-value overload, and "
          "reusing `out` across a shrinking scene clears the stale tail") {
    SceneDocument doc3;
    for (int32_t i = 0; i < 3; ++i) {
        Node n;
        n.id = i + 1;
        n.shape = Shape::Cube;
        n.op = Op::Add;
        n.position = {static_cast<float>(i), 0.0f, 0.0f};
        n.scale = {1.0f, 1.0f, 1.0f};
        doc3.add(n);
    }

    std::vector<SdfNode> out;
    pack_scene(doc3, out);
    const std::vector<SdfNode> expected3 = pack_scene(doc3);
    REQUIRE(out.size() == expected3.size());
    for (size_t i = 0; i < out.size(); ++i) {
        check_float3_approx(out[i].pos_shape.xyz, expected3[i].pos_shape.xyz);
        check_float3_approx(out[i].half_extents_op.xyz, expected3[i].half_extents_op.xyz);
    }

    // Reuse the same `out` (nonempty, capacity already >= 1) for a
    // single-node scene: the leftover entries from the 3-node pack above
    // must not survive -- pack_scene's out.clear() is the guard against a
    // caller (the renderer's per-frame scratch vector) observing stale
    // nodes from a larger previous scene.
    SceneDocument doc1;
    Node n1;
    n1.id = 1;
    n1.shape = Shape::Sphere;
    n1.op = Op::Add;
    n1.position = {9.0f, 9.0f, 9.0f};
    n1.scale = {2.0f, 2.0f, 2.0f};
    doc1.add(n1);

    pack_scene(doc1, out);
    REQUIRE(out.size() == 1u);
    check_float3_approx(out[0].pos_shape.xyz, simd_float3{9.0f, 9.0f, 9.0f});
}

// --- sdf_ray_for_pixel <-> Camera::ray_through_view_point agreement --------

TEST_CASE("sdf_ray_for_pixel agrees with Camera::ray_through_view_point at center, "
          "off-center, and corner pixels") {
    const Camera camera = make_camera(1.6f);
    const float w = 800.0f, h = 600.0f;
    const simd_float4x4 inv_view_proj = simd_inverse(camera.view_proj());

    const std::vector<std::pair<float, float>> pixels = {
        {w * 0.5f, h * 0.5f},  // center
        {0.25f * w, 0.7f * h}, // off-center
        {0.0f, 0.0f},          // corner
    };

    for (const auto& [px, py] : pixels) {
        CAPTURE(px);
        CAPTURE(py);

        const Ray expected = camera.ray_through_view_point(px, py, w, h);
        const SdfRay actual = sdf_ray_for_pixel(px, py, w, h, inv_view_proj);

        check_float3_approx(actual.origin, expected.origin, 1e-4);
        check_float3_approx(actual.dir, expected.dir, 1e-4);
    }
}
