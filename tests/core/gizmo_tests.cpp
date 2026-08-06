#include <doctest.h>

#include <cmath>
#include <simd/simd.h>
#include <utility>
#include <vector>

#include "camera.h"
#include "gizmo.h"
#include "lines.h" // append_move_gizmo_handles, for the drawn == hit case
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

TEST_CASE("gizmo_frame_for_node: snapped Move uses the snap frame; everything else uses the "
          "node's own axes; half_extent is screen-constant") {
    const Camera cam = test_camera();

    Node node;
    node.position = {0.5f, 1.0f, -0.25f};

    SUBCASE("free Move: origin = position, basis = the node's local axes") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
        check_float3_approx(f.origin, node.position);
        // Identity rotation, so the local axes are world X/Y/Z. This used to be
        // a camera-facing basis (-camera_forward and its tangent basis), which
        // is exactly the behaviour being reversed here.
        check_float3_approx(f.u, simd_float3{1.0f, 0.0f, 0.0f});
        check_float3_approx(f.v, simd_float3{0.0f, 1.0f, 0.0f});
        check_float3_approx(f.n, simd_float3{0.0f, 0.0f, 1.0f});
        // ...and its grid is world-horizontal, not the u-v plane.
        check_float3_approx(f.grid_normal, simd_float3{0.0f, 1.0f, 0.0f});
    }

    SUBCASE("snapped Move: origin = snap_point, n = snap_normal, grid = the tangent plane") {
        node.snapped = true;
        node.snap_point = {1.0f, 0.0f, 1.0f};
        node.snap_normal = simd_normalize(simd_float3{0.0f, 1.0f, 0.2f});

        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
        check_float3_approx(f.origin, node.snap_point);
        check_float3_approx(f.n, node.snap_normal);
        check_float3_approx(f.grid_normal, node.snap_normal);

        simd_float3 u, v;
        tangent_basis(node.snap_normal, u, v);
        check_float3_approx(f.u, u);
        check_float3_approx(f.v, v);
    }

    SUBCASE("Scale ignores snapping entirely: always the node's centre and own axes") {
        node.snapped = true;
        node.snap_point = {1.0f, 0.0f, 1.0f};
        node.snap_normal = simd_normalize(simd_float3{0.0f, 1.0f, 0.2f});

        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoSlot::Shape);
        check_float3_approx(f.origin, node.position); // NOT snap_point
        check_float3_approx(f.u, simd_float3{1.0f, 0.0f, 0.0f});
        check_float3_approx(f.v, simd_float3{0.0f, 1.0f, 0.0f});
        check_float3_approx(f.n, simd_float3{0.0f, 0.0f, 1.0f});
    }

    SUBCASE("half_extent = kGizmoScreenFraction * d * 2*tan(fov/2)") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
        const float d = simd_length(node.position - cam.eye);
        const float expected = kGizmoScreenFraction * d * 2.0f * std::tan(cam.fov_y_radians * 0.5f);
        CHECK(f.half_extent == doctest::Approx(expected));
        CHECK(f.half_extent > 0.0f);
    }
}

TEST_CASE("gizmo_frame_for_node: the basis does not depend on the camera") {
    // THE regression test for this rework. A manipulator whose axes swim as you
    // orbit cannot build muscle memory, and an unsnapped node's basis used to
    // be derived straight from camera_forward. Only half_extent (screen-constant
    // sizing) is allowed to vary with the camera, so it is asserted to differ
    // rather than merely left unchecked -- otherwise a frame that ignored the
    // camera completely would pass this case for the wrong reason.
    Camera a = test_camera();
    Camera b = test_camera();
    b.eye = {-7.0f, -4.0f, -2.0f}; // a wholly different pose, far side of the scene
    b.target = {0.5f, 1.0f, -0.25f};

    Node free_node;
    free_node.position = {0.5f, 1.0f, -0.25f};

    Node snapped = free_node;
    snapped.snapped = true;
    snapped.snap_point = {1.0f, 0.0f, 1.0f};
    snapped.snap_normal = simd_normalize(simd_float3{0.3f, 1.0f, 0.2f});

    for (const Node& node : {free_node, snapped}) {
        for (const GizmoSlot slot : {GizmoSlot::Placement, GizmoSlot::Shape}) {
            INFO("snapped: " << node.snapped << ", shape slot: " << (slot == GizmoSlot::Shape));
            const GizmoFrame fa = gizmo_frame_for_node(node, a, slot);
            const GizmoFrame fb = gizmo_frame_for_node(node, b, slot);
            check_float3_approx(fa.origin, fb.origin);
            check_float3_approx(fa.u, fb.u);
            check_float3_approx(fa.v, fb.v);
            check_float3_approx(fa.n, fb.n);
            check_float3_approx(fa.grid_normal, fb.grid_normal);
            CHECK(fa.half_extent != doctest::Approx(fb.half_extent)); // sizing still tracks depth
        }
    }
}

TEST_CASE("gizmo_frame_for_node: the node's own axes follow its rotation") {
    const Camera cam = test_camera();

    Node node;
    node.position = {0.0f, 0.0f, 0.0f};
    // A quarter turn about +Y takes local X to world -Z and local Z to world X.
    node.rotation = simd_quaternion(static_cast<float>(M_PI_2), simd_float3{0.0f, 1.0f, 0.0f});

    for (const GizmoSlot slot : {GizmoSlot::Placement, GizmoSlot::Shape}) {
        INFO("shape slot: " << (slot == GizmoSlot::Shape));
        const GizmoFrame f = gizmo_frame_for_node(node, cam, slot);
        check_float3_approx(f.u, simd_float3{0.0f, 0.0f, -1.0f});
        check_float3_approx(f.v, simd_float3{0.0f, 1.0f, 0.0f});
        check_float3_approx(f.n, simd_float3{1.0f, 0.0f, 0.0f});
        // Still right-handed after the turn, which is what the drag and draw
        // paths both assume.
        check_float3_approx(simd_cross(f.u, f.v), f.n);
    }

    SUBCASE("but a snapped Move frame follows the SURFACE, not the node's spin") {
        node.snapped = true;
        node.snap_point = {0.0f, 0.0f, 0.0f};
        node.snap_normal = {0.0f, 1.0f, 0.0f};
        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
        check_float3_approx(f.n, node.snap_normal);
    }
}

// --- gizmos_coalesce --------------------------------------------------------

TEST_CASE("gizmos_coalesce: the two anchors merge below the threshold and split above it") {
    const Camera cam = test_camera();

    Node node;
    node.position = {0.0f, 0.0f, 0.0f};
    node.snapped = true;
    node.snap_normal = {0.0f, 1.0f, 0.0f};

    const auto pair_for = [&](simd_float3 position, simd_float3 snap_point) {
        Node n = node;
        n.position = position;
        n.snap_point = snap_point;
        return std::pair<GizmoFrame, GizmoFrame>{
            gizmo_frame_for_node(n, cam, GizmoSlot::Placement),
            gizmo_frame_for_node(n, cam, GizmoSlot::Shape)};
    };

    SUBCASE("a freshly spawned detail is coalesced: spawn_snapped puts its centre ON its "
            "snap point, so the two anchors are the same point") {
        const auto [placement, shape] = pair_for(simd_float3{1.0f, 0.0f, 1.0f}, simd_float3{1.0f, 0.0f, 1.0f});
        CHECK(gizmos_coalesce(placement, shape));
    }

    SUBCASE("lifting the node off the surface splits them") {
        // The threshold is a fraction of half_extent, which is screen-constant,
        // so it is derived here rather than written as a world distance.
        const auto [p0, s0] = pair_for(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 0.0f, 0.0f});
        const float he = p0.half_extent;

        const float inside = 0.5f * kGizmoCoalesceFrac * he;
        const auto [p1, s1] = pair_for(simd_float3{0.0f, inside, 0.0f}, simd_float3{0.0f, 0.0f, 0.0f});
        CHECK(gizmos_coalesce(p1, s1));

        const float outside = 2.0f * kGizmoCoalesceFrac * he;
        const auto [p2, s2] = pair_for(simd_float3{0.0f, outside, 0.0f}, simd_float3{0.0f, 0.0f, 0.0f});
        CHECK_FALSE(gizmos_coalesce(p2, s2));
    }

    SUBCASE("a free node is always coalesced: both slots anchor on its centre") {
        Node free_node;
        free_node.position = {2.0f, -1.0f, 0.5f};
        CHECK(gizmos_coalesce(gizmo_frame_for_node(free_node, cam, GizmoSlot::Placement),
                              gizmo_frame_for_node(free_node, cam, GizmoSlot::Shape)));
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

// Test-local convenience; the slot defaults to Placement because most cases
// here predate the second gizmo. The production entry point takes it
// explicitly, and pick_gizmos takes both frames at once.
GizmoHandle pick(const GizmoFrame& f, simd_float3 eye, simd_float3 target,
                 GizmoSlot slot = GizmoSlot::Placement) {
    return pick_gizmo_handle(f, ray_through(eye, target), kTestFov, kTestViewportH, slot);
}

// A scale gizmo's frame: node-local axes (world X/Y/Z at identity rotation),
// matching what gizmo_frame_for_node builds for GizmoSlot::Shape.
GizmoFrame make_scale_frame(simd_float3 origin = simd_float3{0.0f, 0.0f, 0.0f},
                            float half_extent = 1.5f) {
    GizmoFrame f;
    f.origin = origin;
    f.u = {1.0f, 0.0f, 0.0f};
    f.v = {0.0f, 1.0f, 0.0f};
    f.n = {0.0f, 0.0f, 1.0f};
    f.half_extent = half_extent;
    return f;
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
        CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH, GizmoSlot::Placement) != GizmoHandle::AxisU);
    }
}

TEST_CASE("pick_gizmo_handle: each axis picked through a point on it; origin tie-break is AxisU") {
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;
    const simd_float3 eye = {2.0f, 3.0f, 5.0f};

    // Mid-band, so the point is on the drawn shaft rather than past its end:
    // Placement's axes stop at kMoveAxisOuterFrac now that Shape's own axes own
    // everything outboard of kScaleAxisInnerFrac.
    const float mid = 0.5f * kMoveAxisOuterFrac;
    CHECK(pick(f, eye, f.origin + mid * he * f.u) == GizmoHandle::AxisU);
    CHECK(pick(f, eye, f.origin + mid * he * f.v) == GizmoHandle::AxisV);
    // n from an eye whose view of the n axis is well off end-on.
    CHECK(pick(f, simd_float3{3.0f, 2.0f, 5.0f}, f.origin + mid * he * f.n) == GizmoHandle::AxisN);

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
    // Every probe below is derived from kGizmoPatchInner/Outer rather than
    // spelled out: these numbers moved once already (the [0.3, 0.6] -> [0.24,
    // 0.50] restyle), and the literal 0.25 that used to sit safely outside the
    // patch silently ended up INSIDE it.
    const float c = kGizmoPatchCenter;
    CHECK(pick(f, simd_float3{0.8f, 0.3f, 6.0f}, f.origin + c * he * (f.u + f.v)) == GizmoHandle::PlaneUV);
    CHECK(pick(f, simd_float3{0.3f, 5.0f, 4.0f}, f.origin + c * he * (f.u + f.n)) == GizmoHandle::PlaneUN);
    CHECK(pick(f, simd_float3{5.0f, 0.3f, 4.0f}, f.origin + c * he * (f.v + f.n)) == GizmoHandle::PlaneVN);
    const simd_float3 eye = {0.8f, 0.3f, 6.0f}; // uv-patch / bounds cases below

    // Patch bounds, just inside and just outside each edge.
    const float inside = kGizmoPatchInner + 0.02f;
    const float below  = kGizmoPatchInner - 0.02f;
    const float above  = kGizmoPatchOuter + 0.02f;
    CHECK(pick(f, eye, f.origin + inside * he * f.u + inside * he * f.v) == GizmoHandle::PlaneUV);
    CHECK(pick(f, eye, f.origin + below * he * f.u + c * he * f.v) == GizmoHandle::None);
    CHECK(pick(f, eye, f.origin + above * he * f.u + c * he * f.v) == GizmoHandle::None);

    // Off everything.
    CHECK(pick(f, eye, f.origin + 3.0f * he * f.u + 3.0f * he * f.v) == GizmoHandle::None);
}

TEST_CASE("pick_gizmo_handle: the patch it hit-tests is the patch lines.cpp draws") {
    // Guards the "drawn = hit" invariant directly, rather than by inspection:
    // walk the emitted uv-patch FILL vertices (the first 6 of that patch's
    // block) and confirm every one lies within the pickable bounds. If a
    // restyle moved the drawn quad without moving the pick, this fails.
    const GizmoFrame f = test_frame();
    const float he = f.half_extent;
    std::vector<LineVertex> out;
    append_move_gizmo_handles(out, f, GizmoHandle::None, simd_float3{0.8f, 0.3f, 6.0f}, kGizmoHandleRestAlpha);
    REQUIRE(out.size() == 132);

    for (size_t i = 36; i < 42; ++i) { // uv patch fill
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        const float x = simd_dot(p - f.origin, f.u);
        const float y = simd_dot(p - f.origin, f.v);
        CHECK(x >= kGizmoPatchInner * he - 1e-5f);
        CHECK(x <= kGizmoPatchOuter * he + 1e-5f);
        CHECK(y >= kGizmoPatchInner * he - 1e-5f);
        CHECK(y <= kGizmoPatchOuter * he + 1e-5f);
    }
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
    CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH, GizmoSlot::Placement) == GizmoHandle::AxisU);
}

TEST_CASE("pick_gizmo_handle: gizmo entirely behind the ray origin is unpickable "
          "(forward-clamped math, no special guard)") {
    const GizmoFrame f = test_frame();
    const simd_float3 eye = {0.2f, 0.1f, 5.0f};
    const Ray away{eye, simd_normalize(eye - f.origin)}; // pointing away from the gizmo
    CHECK(pick_gizmo_handle(f, away, kTestFov, kTestViewportH, GizmoSlot::Placement) == GizmoHandle::None);
}

TEST_CASE("pivot_marker_alpha: hold at full, then smoothstep to nothing") {
    SUBCASE("full strength through the hold window") {
        CHECK(pivot_marker_alpha(0.0f) == doctest::Approx(1.0f));
        CHECK(pivot_marker_alpha(kPivotHoldSeconds * 0.5f) == doctest::Approx(1.0f));
        CHECK(pivot_marker_alpha(kPivotHoldSeconds) == doctest::Approx(1.0f));
    }

    SUBCASE("gone once the fade completes, and stays gone") {
        CHECK(pivot_marker_alpha(kPivotHoldSeconds + kPivotFadeSeconds) == doctest::Approx(0.0f));
        CHECK(pivot_marker_alpha(10.0f) == doctest::Approx(0.0f));
        // The resting state: Editor seeds last_camera_activity 10s in the past
        // precisely so the marker starts hidden.
        CHECK(pivot_marker_alpha(1000.0f) == doctest::Approx(0.0f));
    }

    SUBCASE("halfway through the fade is halfway down") {
        CHECK(pivot_marker_alpha(kPivotHoldSeconds + 0.5f * kPivotFadeSeconds)
              == doctest::Approx(0.5f));
    }

    SUBCASE("monotonic non-increasing across the whole range") {
        float prev = 2.0f;
        for (int i = 0; i <= 100; ++i) {
            const float t = static_cast<float>(i) * 0.01f;
            const float a = pivot_marker_alpha(t);
            CAPTURE(t);
            CHECK(a <= prev + 1e-6f);
            CHECK(a >= 0.0f);
            CHECK(a <= 1.0f);
            prev = a;
        }
    }

    SUBCASE("a negative elapsed time is treated as 'just moved', not as a fade") {
        CHECK(pivot_marker_alpha(-1.0f) == doctest::Approx(1.0f));
    }
}

// --- scale gizmo -----------------------------------------------------------

TEST_CASE("gizmo_frame_for_node: the scale frame is node-local, not the drag plane") {
    Camera cam;
    cam.eye = {0.0f, 0.0f, 6.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fov_y_radians = kTestFov;
    cam.aspect = 1.6f;

    // A snapped node: its MOVE frame follows the snap normal, which is exactly
    // the frame a scale gizmo must NOT use — scale handles have to land on
    // scale components, and a tangent basis maps to none of them.
    Node node;
    node.position = {1.0f, 2.0f, 0.0f};
    node.snapped = true;
    node.snap_point = {1.0f, 1.5f, 0.0f};
    node.snap_normal = simd_normalize(simd_float3{1.0f, 1.0f, 0.0f});

    const GizmoFrame move = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
    const GizmoFrame scale = gizmo_frame_for_node(node, cam, GizmoSlot::Shape);

    check_float3_approx(move.origin, node.snap_point); // move rides the snap frame
    check_float3_approx(scale.origin, node.position);  // scale is centred on the node

    check_float3_approx(scale.u, simd_float3{1.0f, 0.0f, 0.0f});
    check_float3_approx(scale.v, simd_float3{0.0f, 1.0f, 0.0f});
    check_float3_approx(scale.n, simd_float3{0.0f, 0.0f, 1.0f});
    // Right-handed like every GizmoFrame: u x v == n.
    check_float3_approx(simd_cross(scale.u, scale.v), scale.n);

    SUBCASE("the scale frame ignores the camera entirely for an unsnapped node") {
        Node loose;
        loose.position = {1.0f, 2.0f, 0.0f};
        loose.snapped = false;

        Camera other = cam;
        other.eye = {5.0f, -4.0f, 2.0f};
        const GizmoFrame a = gizmo_frame_for_node(loose, cam, GizmoSlot::Shape);
        const GizmoFrame b = gizmo_frame_for_node(loose, other, GizmoSlot::Shape);
        check_float3_approx(a.u, b.u);
        check_float3_approx(a.v, b.v);
        check_float3_approx(a.n, b.n);
        // Only the screen-constant size tracks the camera.
        CHECK(a.half_extent != doctest::Approx(b.half_extent));
    }
}

TEST_CASE("pick_gizmo_handle (Scale): centre owns the middle, axes own outboard, no planes") {
    const GizmoFrame f = make_scale_frame();
    const simd_float3 eye = {0.0f, 0.0f, 8.0f};

    SUBCASE("dead centre is the uniform handle, never an axis") {
        CHECK(pick(f, eye, f.origin, GizmoSlot::Shape) == GizmoHandle::Uniform);
    }

    SUBCASE("each axis is picked through a point on its drawn shaft") {
        const float mid = 0.5f * (kScaleAxisInnerFrac + kScaleAxisShaftFrac) * f.half_extent;
        CHECK(pick(f, eye, f.origin + mid * f.u, GizmoSlot::Shape) == GizmoHandle::AxisU);
        CHECK(pick(f, eye, f.origin + mid * f.v, GizmoSlot::Shape) == GizmoHandle::AxisV);
        // n points at the eye here, so aim from off-axis for a usable view of it.
        CHECK(pick(f, simd_float3{7.0f, 1.0f, 1.0f}, f.origin + mid * f.n, GizmoSlot::Shape) ==
              GizmoHandle::AxisN);
    }

    SUBCASE("the scale gizmo has no plane handles where the move gizmo does") {
        // Dead centre of the uv patch: a real handle under Move, nothing under
        // Scale (the ray still misses the axes out there).
        const simd_float3 patch =
            f.origin + kGizmoPatchCenter * f.half_extent * (f.u + f.v);
        CHECK(pick(f, eye, patch, GizmoSlot::Placement) == GizmoHandle::PlaneUV);
        CHECK(pick(f, eye, patch, GizmoSlot::Shape) == GizmoHandle::None);
    }

    SUBCASE("scale's wider tolerance catches grabs that move's would drop") {
        // Offset perpendicular to the u axis by more than Move's 8pt tolerance
        // but less than Scale's 14pt, at the same depth — the near-miss the
        // wider tolerance exists for, now that a miss drives the camera.
        const float mid = 0.5f * (kScaleAxisInnerFrac + kScaleAxisShaftFrac) * f.half_extent;
        const simd_float3 on_axis = f.origin + mid * f.u;
        const float pts_to_world = 2.0f * std::tan(kTestFov * 0.5f) / kTestViewportH;
        const float world_per_pt = simd_length(on_axis - eye) * pts_to_world;
        const simd_float3 near_miss = on_axis + (11.0f * world_per_pt) * f.v;

        CHECK(pick(f, eye, near_miss, GizmoSlot::Shape) == GizmoHandle::AxisU);
        CHECK(pick(f, eye, near_miss, GizmoSlot::Placement) != GizmoHandle::AxisU);
    }
}

TEST_CASE("pick_gizmo_handle (Scale): the shafts it hit-tests are the shafts lines.cpp draws") {
    // Same drawn == hit invariant the move gizmo's patch case pins, for the
    // segment bounds that moved when the centre box took over the middle.
    const GizmoFrame f = make_scale_frame();
    const simd_float3 eye = {0.8f, 0.3f, 8.0f};

    std::vector<LineVertex> out;
    append_scale_gizmo_handles(out, f, GizmoHandle::None, eye, kGizmoHandleRestAlpha);
    CHECK(out.size() == 42); // 3 shafts + 3 tips + 1 centre box, 6 verts each

    // Dead centre belongs to the uniform handle, never an axis: the shafts
    // start at kScaleAxisInnerFrac and the centre owns the disc inside that.
    CHECK(pick(f, eye, f.origin, GizmoSlot::Shape) == GizmoHandle::Uniform);
    // And a point in the band's inner GAP -- past the uniform handle's grab
    // radius but short of the drawn shaft -- hits nothing on this gizmo, which
    // is what leaves it free for Placement's axes when the two are coalesced.
    const simd_float3 gap = f.origin + 0.5f * kScaleAxisInnerFrac * f.half_extent * f.u;
    CHECK(pick(f, eye, gap, GizmoSlot::Shape) == GizmoHandle::None);
}

TEST_CASE("scale_axis_param floors the parameter so the drag ratio stays sane") {
    const GizmoFrame f = make_scale_frame();
    const float floor_s = kScaleAxisMinGrabFrac * f.half_extent;

    SUBCASE("a grab out on the shaft is returned unfloored") {
        const simd_float3 target = f.origin + 0.8f * f.half_extent * f.u;
        const Ray ray = ray_through(simd_float3{0.0f, 0.0f, 8.0f}, target);
        const auto s = scale_axis_param(ray, f, GizmoHandle::AxisU);
        REQUIRE(s.has_value());
        CHECK(*s == doctest::Approx(0.8f * f.half_extent));
    }

    SUBCASE("aiming at, or past, the origin saturates instead of hitting zero or flipping") {
        for (const float along : {0.01f, 0.0f, -0.5f, -5.0f}) {
            CAPTURE(along);
            const simd_float3 target = f.origin + along * f.half_extent * f.u;
            const Ray ray = ray_through(simd_float3{0.0f, 0.0f, 8.0f}, target);
            const auto s = scale_axis_param(ray, f, GizmoHandle::AxisU);
            REQUIRE(s.has_value());
            CHECK(*s == doctest::Approx(floor_s));
            CHECK(*s > 0.0f); // the property the ratio depends on
        }
    }

    SUBCASE("start and update use the same floor, so a grab begins at factor 1") {
        // The reason the floor is applied on BOTH sides rather than only at
        // capture: an asymmetric floor would make s_now/s_start != 1 on
        // mouse-down and jump the node before any drag happened.
        const simd_float3 target = f.origin + 0.005f * f.half_extent * f.u;
        const Ray ray = ray_through(simd_float3{0.0f, 0.0f, 8.0f}, target);
        const auto start = scale_axis_param(ray, f, GizmoHandle::AxisU);
        const auto now = scale_axis_param(ray, f, GizmoHandle::AxisU);
        REQUIRE(start.has_value());
        REQUIRE(now.has_value());
        CHECK((*now / *start) == doctest::Approx(1.0f));
    }
}

TEST_CASE("a grab at the innermost pickable point still has real shrink range") {
    // The failure this pins: the most a drag can shrink an axis is
    // floor / s_start, and s_start is bounded below by the innermost grabbable
    // point. With the floor set close to that bound (0.15 against an inner
    // bound of 0.18) the worst case was 0.83x -- dragging inboard from the
    // inner end of the shaft barely moved the node and read as a dead gesture.
    const GizmoFrame f = make_scale_frame();
    const simd_float3 eye = {0.0f, 0.0f, 8.0f};

    // Grab as far inboard as the pick will allow, then drag through the origin.
    const simd_float3 grab = f.origin + kScaleAxisInnerFrac * f.half_extent * f.u;
    const auto start = scale_axis_param(ray_through(eye, grab), f, GizmoHandle::AxisU);
    REQUIRE(start.has_value());

    const simd_float3 pull = f.origin - 5.0f * f.half_extent * f.u; // well past the origin
    const auto now = scale_axis_param(ray_through(eye, pull), f, GizmoHandle::AxisU);
    REQUIRE(now.has_value());

    const float min_factor = *now / *start;
    CHECK(min_factor > 0.0f);   // still positive: no flip
    CHECK(min_factor < 0.2f);   // and genuinely useful range, not ~0.83
}

TEST_CASE("gizmo_scale_axis_index maps u/v/n onto scale.x/y/z, and nothing else") {
    CHECK(gizmo_scale_axis_index(GizmoHandle::AxisU) == 0);
    CHECK(gizmo_scale_axis_index(GizmoHandle::AxisV) == 1);
    CHECK(gizmo_scale_axis_index(GizmoHandle::AxisN) == 2);
    for (const GizmoHandle h : {GizmoHandle::None, GizmoHandle::Uniform, GizmoHandle::PlaneUV,
                                GizmoHandle::PlaneUN, GizmoHandle::PlaneVN}) {
        CHECK(gizmo_scale_axis_index(h) == -1);
    }
    // Uniform drives a screen-space drag, not an axis solve, so anything
    // branching on "is this an axis?" must route it elsewhere.
    CHECK_FALSE(gizmo_handle_is_axis(GizmoHandle::Uniform));
}

// --- pick_gizmos: resolving between the two ---------------------------------

TEST_CASE("pick_gizmos: a coalesced pair resolves by radius band") {
    // Both gizmos on one origin with the same basis -- the hardest case for the
    // resolution rule, because every handle of both is in play along the same
    // three lines. The bands are what keep them apart.
    const Camera cam = test_camera();
    Node node;
    node.position = {0.0f, 0.0f, 0.0f};

    const GizmoFrame placement = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
    const GizmoFrame shape = gizmo_frame_for_node(node, cam, GizmoSlot::Shape);
    REQUIRE(gizmos_coalesce(placement, shape));
    const float he = placement.half_extent;
    const simd_float3 eye = cam.eye;

    const auto hit_at = [&](simd_float3 target) {
        return pick_gizmos(placement, shape, ray_through(eye, target), cam.fov_y_radians,
                           kTestViewportH);
    };

    SUBCASE("the centre is Shape's uniform handle") {
        const GizmoHit hit = hit_at(placement.origin);
        CHECK(hit.slot == GizmoSlot::Shape);
        CHECK(hit.handle == GizmoHandle::Uniform);
    }

    SUBCASE("the inner shaft is Placement's move axis") {
        const GizmoHit hit = hit_at(placement.origin + 0.5f * kMoveAxisOuterFrac * he * placement.u);
        CHECK(hit.slot == GizmoSlot::Placement);
        CHECK(hit.handle == GizmoHandle::AxisU);
    }

    SUBCASE("the outer shaft is Shape's scale axis") {
        const float mid = 0.5f * (kScaleAxisInnerFrac + kScaleAxisShaftFrac);
        const GizmoHit hit = hit_at(shape.origin + mid * he * shape.u);
        CHECK(hit.slot == GizmoSlot::Shape);
        CHECK(hit.handle == GizmoHandle::AxisU);
    }

    SUBCASE("the plane patch is Placement's, and loses to no axis out there") {
        const GizmoHit hit =
            hit_at(placement.origin + kGizmoPatchCenter * he * (placement.u + placement.v));
        CHECK(hit.slot == GizmoSlot::Placement);
        CHECK(hit.handle == GizmoHandle::PlaneUV);
    }

    SUBCASE("well clear of everything hits nothing") {
        const GizmoHit hit = hit_at(placement.origin + 4.0f * he * placement.u);
        CHECK(hit.handle == GizmoHandle::None);
    }
}

TEST_CASE("pick_gizmos: a split pair is picked at whichever anchor the ray is near") {
    const Camera cam = test_camera();
    Node node;
    node.snapped = true;
    node.snap_point = {0.0f, 0.0f, 0.0f};
    node.snap_normal = {0.0f, 1.0f, 0.0f};
    node.position = {0.0f, 2.5f, 0.0f}; // lifted well clear of the surface

    const GizmoFrame placement = gizmo_frame_for_node(node, cam, GizmoSlot::Placement);
    const GizmoFrame shape = gizmo_frame_for_node(node, cam, GizmoSlot::Shape);
    REQUIRE_FALSE(gizmos_coalesce(placement, shape));

    const auto hit_at = [&](simd_float3 target) {
        return pick_gizmos(placement, shape, ray_through(cam.eye, target), cam.fov_y_radians,
                           kTestViewportH);
    };

    // Down at the surface: only Placement is there.
    const GizmoHit low =
        hit_at(placement.origin + 0.5f * kMoveAxisOuterFrac * placement.half_extent * placement.u);
    CHECK(low.slot == GizmoSlot::Placement);
    CHECK(low.handle == GizmoHandle::AxisU);

    // Up at the node's centre: only Shape is there.
    const GizmoHit high = hit_at(shape.origin);
    CHECK(high.slot == GizmoSlot::Shape);
    CHECK(high.handle == GizmoHandle::Uniform);
}
