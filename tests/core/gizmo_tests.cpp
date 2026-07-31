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

// --- ray_axis_param / pick_gizmo_handle -------------------------------------
//
// Fixed test frame: origin {0,0,0}, n = {0,0,1}. tangent_basis gives
// u = {-1,0,0} (x-axis line), v = {0,-1,0} (y-axis line); n is the z-axis
// line. he = 1.5. fov/viewport match the Editor test camera (1.0472, 500pts),
// so the world tolerance at eye distance ~5-6 is ~0.09-0.13 — the offsets
// below are chosen with comfortable margin on the right side of it.

namespace {

constexpr float kTestFov = 1.0472f;
constexpr float kTestViewportH = 500.0f;

GizmoFrame test_frame() {
    GizmoFrame f;
    f.origin = {0.0f, 0.0f, 0.0f};
    f.n = {0.0f, 0.0f, 1.0f};
    tangent_basis(f.n, f.u, f.v);
    f.half_extent = 1.5f;
    return f;
}

Ray ray_through(simd_float3 eye, simd_float3 target) {
    return Ray{eye, simd_normalize(target - eye)};
}

GizmoHandle pick(const GizmoFrame& f, simd_float3 eye, simd_float3 target) {
    return pick_gizmo_handle(f, ray_through(eye, target), kTestFov, kTestViewportH);
}

} // namespace

TEST_CASE("ray_axis_param: recovers the axis parameter of a point the ray passes through, "
          "and rejects near-parallel rays") {
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;

    const simd_float3 on_axis = f.origin + 0.8f * he * f.u;
    const std::optional<float> s = ray_axis_param(ray_through(simd_float3{2.0f, 3.0f, 5.0f}, on_axis), f.origin, f.u);
    REQUIRE(s.has_value());
    CHECK(*s == doctest::Approx(0.8f * he));

    // Parallel: dir == u exactly, from off the axis.
    CHECK_FALSE(ray_axis_param(Ray{{0.0f, 1.0f, 0.0f}, f.u}, f.origin, f.u).has_value());
}

TEST_CASE("ray_axis_param/pick: near-parallel fuzz — finite or rejected, and the view-angle "
          "guard keeps the axis unpickable well before the solver degenerates") {
    const GizmoFrame f = test_frame();

    for (const float angle : {0.002f, 0.01f, 0.05f}) {
        CAPTURE(angle);
        // Ray tilted `angle` radians off the u axis, passing near the axis tip.
        const simd_float3 dir = simd_normalize(std::cos(angle) * f.u + std::sin(angle) * f.n);
        const Ray ray{f.origin - 3.0f * f.u + simd_float3{0.0f, 0.0f, 0.01f}, dir};

        const std::optional<float> s = ray_axis_param(ray, f.origin, f.u);
        if (s.has_value()) {
            CHECK(std::isfinite(*s));
        }
        // cos(0.05) ~= 0.99875 > kAxisViewAlignLimit: all three fuzz angles are
        // inside the guard, so the u axis must never be the pick result.
        CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH) != GizmoHandle::AxisU);
    }
}

TEST_CASE("pick_gizmo_handle: each axis picked through a point on it; origin tie-break is AxisU") {
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;
    const simd_float3 eye = {2.0f, 3.0f, 5.0f};

    CHECK(pick(f, eye, f.origin + 0.8f * he * f.u) == GizmoHandle::AxisU);
    CHECK(pick(f, eye, f.origin + 0.8f * he * f.v) == GizmoHandle::AxisV);
    // n from an eye whose view of the n axis is well off end-on.
    CHECK(pick(f, simd_float3{3.0f, 2.0f, 5.0f}, f.origin + 0.8f * he * f.n) == GizmoHandle::AxisN);

    // Through the origin every axis is at distance 0 with the same ray-t:
    // deterministic declaration-order tie-break.
    CHECK(pick(f, eye, f.origin) == GizmoHandle::AxisU);

    // Axes are positive-only (R3): the negative half is neither drawn nor
    // pickable — a ray through -0.8he*u clamps to the origin endpoint, far
    // outside tolerance, and the negative quadrant holds no patches either.
    CHECK(pick(f, eye, f.origin - 0.8f * he * f.u) == GizmoHandle::None);
}

TEST_CASE("pick_gizmo_handle: plane patches hit at their centers, bounds respected, "
          "clean miss is None") {
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;
    // Each patch gets an eye well OFF its own plane so the ray meets it
    // steeply: a ray that hugs a patch's plane also hugs the axes lying in
    // that plane and axis-priority (correctly) steals the pick — an eye at
    // y=0.3 aiming at the u-n patch (in y=0) grazes the u axis at ~0.04,
    // inside tolerance. Asymmetric x/y also avoids accidentally crossing the
    // n axis (an x==y eye aimed at the uv patch center does exactly that).
    CHECK(pick(f, simd_float3{0.8f, 0.3f, 6.0f}, f.origin + 0.45f * he * (f.u + f.v)) == GizmoHandle::PlaneUV);
    CHECK(pick(f, simd_float3{0.3f, 5.0f, 4.0f}, f.origin + 0.45f * he * (f.u + f.n)) == GizmoHandle::PlaneUN);
    CHECK(pick(f, simd_float3{5.0f, 0.3f, 4.0f}, f.origin + 0.45f * he * (f.v + f.n)) == GizmoHandle::PlaneVN);
    const simd_float3 eye = {0.8f, 0.3f, 6.0f}; // uv-patch / bounds cases below

    // Patch bounds [0.3he, 0.6he] per coordinate.
    CHECK(pick(f, eye, f.origin + 0.35f * he * f.u + 0.35f * he * f.v) == GizmoHandle::PlaneUV);
    CHECK(pick(f, eye, f.origin + 0.25f * he * f.u + 0.45f * he * f.v) == GizmoHandle::None);
    CHECK(pick(f, eye, f.origin + 0.65f * he * f.u + 0.45f * he * f.v) == GizmoHandle::None);

    // Off everything.
    CHECK(pick(f, eye, f.origin + 3.0f * he * f.u + 3.0f * he * f.v) == GizmoHandle::None);
}

TEST_CASE("pick_gizmo_handle: an axis offset beyond the world tolerance at its depth "
          "does not pick, and falls through to None") {
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;
    // 0.4 world units off the u axis along n: the ray's line-line distance to
    // the axis is ~0.22, roughly double the ~0.11 tolerance at depth ~5.5.
    CHECK(pick(f, simd_float3{2.0f, 3.0f, 5.0f}, f.origin + 0.8f * he * f.u + simd_float3{0.0f, 0.0f, 0.4f})
          == GizmoHandle::None);
}

TEST_CASE("pick_gizmo_handle: a ray grazing an axis while crossing a plane patch "
          "prefers the axis") {
    const GizmoFrame f = test_frame();
    // Shallow ray almost along -n from just above the u axis: passes ~0.013
    // from the u-axis line (within tolerance) and then lands inside the u-n
    // patch at (u,n) ~ (0.675, 0.7). Axis priority must win.
    const Ray ray = ray_through(simd_float3{-0.675f, 0.08f, 5.0f}, simd_float3{-0.675f, 0.0f, 0.7f});
    CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH) == GizmoHandle::AxisU);
}

TEST_CASE("pick_gizmo_handle: gizmo entirely behind the ray origin is unpickable "
          "(forward-clamped math, no special guard)") {
    const GizmoFrame f = test_frame();
    const simd_float3 eye = {0.2f, 0.1f, 5.0f};
    const Ray away{eye, simd_normalize(eye - f.origin)}; // pointing away from the gizmo
    CHECK(pick_gizmo_handle(f, away, kTestFov, kTestViewportH) == GizmoHandle::None);
}
