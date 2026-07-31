#include <doctest.h>

#include <cmath>
#include <simd/simd.h>

#include "camera.h"
#include "gizmo.h"
#include "scene.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components — same pattern as drag_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Orthonormal + right-handed: u,v unit, all pairs orthogonal, u x v == n.
void check_basis(const simd_float3 n, const simd_float3 u, const simd_float3 v) {
    CHECK(simd_length(u) == doctest::Approx(1.0f));
    CHECK(simd_length(v) == doctest::Approx(1.0f));
    CHECK(simd_dot(u, v) == doctest::Approx(0.0f));
    CHECK(simd_dot(u, n) == doctest::Approx(0.0f));
    CHECK(simd_dot(v, n) == doctest::Approx(0.0f));
    check_float3_approx(simd_cross(u, v), n);
}

// The camera drag_tests.cpp documents for Editor::create(), reconstructed
// from the same literals (see that file's header comment).
Camera test_camera() {
    Camera cam;
    cam.eye = {4.0f, 3.0f, 6.0f};
    cam.target = {0.0f, 0.5f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fov_y_radians = 1.0472f;
    cam.aspect = 800.0f / 500.0f;
    return cam;
}

} // namespace

// --- tangent_basis ----------------------------------------------------------

TEST_CASE("tangent_basis: orthonormal right-handed frame on both ref branches, "
          "matching the documented formula") {
    const simd_float3 cases[] = {
        {0.0f, 1.0f, 0.0f},                                  // |n.y| >= 0.99 branch
        simd_normalize(simd_float3{1.0f, 2.0f, 0.5f}),       // tilted
        simd_normalize(simd_float3{-0.3f, -0.9f, 0.2f}),     // negative-heavy, |n.y| < 0.99
        simd_normalize(simd_float3{0.99f, 0.1f, 0.0f}),      // X-skewed
        {0.0f, -1.0f, 0.0f},                                 // negative pole, |n.y| >= 0.99 branch
    };
    for (const simd_float3 n : cases) {
        CAPTURE(n.x); CAPTURE(n.y); CAPTURE(n.z);
        simd_float3 u, v;
        tangent_basis(n, u, v);
        check_basis(n, u, v);

        // Pin the exact documented formula (lines.h), recomputed independently.
        const simd_float3 ref = (std::fabs(n.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f}
                                                         : simd_float3{1.0f, 0.0f, 0.0f};
        check_float3_approx(u, simd_normalize(simd_cross(n, ref)));
        check_float3_approx(v, simd_cross(n, u));
    }
}

TEST_CASE("tangent_basis: degenerate zero normal falls back to {0,1,0} instead of NaNs") {
    simd_float3 u, v;
    tangent_basis(simd_float3{0.0f, 0.0f, 0.0f}, u, v);

    const simd_float3 fallback_n = {0.0f, 1.0f, 0.0f};
    check_basis(fallback_n, u, v);
    CHECK(std::isfinite(u.x)); CHECK(std::isfinite(u.y)); CHECK(std::isfinite(u.z));
    CHECK(std::isfinite(v.x)); CHECK(std::isfinite(v.y)); CHECK(std::isfinite(v.z));
}

// --- gizmo_frame_for_node ---------------------------------------------------

TEST_CASE("gizmo_frame_for_node: snapped node uses the snap frame; unsnapped faces the camera; "
          "half_extent is screen-constant") {
    const Camera cam = test_camera();
    const simd_float3 forward = simd_normalize(cam.target - cam.eye);

    Node node;
    node.position = {0.5f, 1.0f, -0.25f};

    SUBCASE("unsnapped: origin = position, n = -camera_forward") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam);
        check_float3_approx(f.origin, node.position);
        check_float3_approx(f.n, -forward);

        simd_float3 u, v;
        tangent_basis(-forward, u, v);
        check_float3_approx(f.u, u);
        check_float3_approx(f.v, v);
    }

    SUBCASE("snapped: origin = snap_point, n = snap_normal") {
        node.snapped = true;
        node.snap_point = {1.0f, 0.0f, 1.0f};
        node.snap_normal = simd_normalize(simd_float3{0.0f, 1.0f, 0.2f});

        const GizmoFrame f = gizmo_frame_for_node(node, cam);
        check_float3_approx(f.origin, node.snap_point);
        check_float3_approx(f.n, node.snap_normal);
    }

    SUBCASE("half_extent = kGizmoScreenFraction * d * 2*tan(fov/2)") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam);
        const float d = simd_length(node.position - cam.eye);
        const float expected = kGizmoScreenFraction * d * 2.0f * std::tan(cam.fov_y_radians * 0.5f);
        CHECK(f.half_extent == doctest::Approx(expected));
        CHECK(f.half_extent > 0.0f);
    }
}
