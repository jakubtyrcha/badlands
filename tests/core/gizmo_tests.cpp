#include <doctest.h>

#include <cmath>
#include <simd/simd.h>
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

TEST_CASE("gizmo_frame_for_node: snapped node uses the snap frame; unsnapped faces the camera; "
          "half_extent is screen-constant") {
    const Camera cam = test_camera();
    const simd_float3 forward = simd_normalize(cam.target - cam.eye);

    Node node;
    node.position = {0.5f, 1.0f, -0.25f};

    SUBCASE("unsnapped: origin = position, n = -camera_forward") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoKind::Move);
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

        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoKind::Move);
        check_float3_approx(f.origin, node.snap_point);
        check_float3_approx(f.n, node.snap_normal);
    }

    SUBCASE("half_extent = kGizmoScreenFraction * d * 2*tan(fov/2)") {
        const GizmoFrame f = gizmo_frame_for_node(node, cam, GizmoKind::Move);
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

// Test-local convenience; the kind defaults to Move because most cases here
// predate the scale gizmo. The production entry point takes it explicitly.
GizmoHandle pick(const GizmoFrame& f, simd_float3 eye, simd_float3 target,
                 GizmoKind kind = GizmoKind::Move) {
    return pick_gizmo_handle(f, ray_through(eye, target), kTestFov, kTestViewportH, kind);
}

// A scale gizmo's frame: node-local axes (world X/Y/Z at identity rotation),
// matching what gizmo_frame_for_node builds for GizmoKind::Scale.
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
        CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH, GizmoKind::Move) != GizmoHandle::AxisU);
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
    append_move_gizmo_handles(out, f, GizmoHandle::None, simd_float3{0.8f, 0.3f, 6.0f});
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
    CHECK(pick_gizmo_handle(f, ray, kTestFov, kTestViewportH, GizmoKind::Move) == GizmoHandle::AxisU);
}

TEST_CASE("pick_gizmo_handle: gizmo entirely behind the ray origin is unpickable "
          "(forward-clamped math, no special guard)") {
    const GizmoFrame f = test_frame();
    const simd_float3 eye = {0.2f, 0.1f, 5.0f};
    const Ray away{eye, simd_normalize(eye - f.origin)}; // pointing away from the gizmo
    CHECK(pick_gizmo_handle(f, away, kTestFov, kTestViewportH, GizmoKind::Move) == GizmoHandle::None);
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

    const GizmoFrame move = gizmo_frame_for_node(node, cam, GizmoKind::Move);
    const GizmoFrame scale = gizmo_frame_for_node(node, cam, GizmoKind::Scale);

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
        const GizmoFrame a = gizmo_frame_for_node(loose, cam, GizmoKind::Scale);
        const GizmoFrame b = gizmo_frame_for_node(loose, other, GizmoKind::Scale);
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
        CHECK(pick(f, eye, f.origin, GizmoKind::Scale) == GizmoHandle::Uniform);
    }

    SUBCASE("each axis is picked through a point on its drawn shaft") {
        const float mid = 0.5f * (kScaleAxisInnerFrac + kGizmoAxisShaftFrac) * f.half_extent;
        CHECK(pick(f, eye, f.origin + mid * f.u, GizmoKind::Scale) == GizmoHandle::AxisU);
        CHECK(pick(f, eye, f.origin + mid * f.v, GizmoKind::Scale) == GizmoHandle::AxisV);
        // n points at the eye here, so aim from off-axis for a usable view of it.
        CHECK(pick(f, simd_float3{7.0f, 1.0f, 1.0f}, f.origin + mid * f.n, GizmoKind::Scale) ==
              GizmoHandle::AxisN);
    }

    SUBCASE("the scale gizmo has no plane handles where the move gizmo does") {
        // Dead centre of the uv patch: a real handle under Move, nothing under
        // Scale (the ray still misses the axes out there).
        const simd_float3 patch =
            f.origin + kGizmoPatchCenter * f.half_extent * (f.u + f.v);
        CHECK(pick(f, eye, patch, GizmoKind::Move) == GizmoHandle::PlaneUV);
        CHECK(pick(f, eye, patch, GizmoKind::Scale) == GizmoHandle::None);
    }

    SUBCASE("scale's wider tolerance catches grabs that move's would drop") {
        // Offset perpendicular to the u axis by more than Move's 8pt tolerance
        // but less than Scale's 14pt, at the same depth — the near-miss the
        // wider tolerance exists for, now that a miss drives the camera.
        const float mid = 0.5f * (kScaleAxisInnerFrac + kGizmoAxisShaftFrac) * f.half_extent;
        const simd_float3 on_axis = f.origin + mid * f.u;
        const float pts_to_world = 2.0f * std::tan(kTestFov * 0.5f) / kTestViewportH;
        const float world_per_pt = simd_length(on_axis - eye) * pts_to_world;
        const simd_float3 near_miss = on_axis + (11.0f * world_per_pt) * f.v;

        CHECK(pick(f, eye, near_miss, GizmoKind::Scale) == GizmoHandle::AxisU);
        CHECK(pick(f, eye, near_miss, GizmoKind::Move) != GizmoHandle::AxisU);
    }
}

TEST_CASE("pick_gizmo_handle (Scale): the shafts it hit-tests are the shafts lines.cpp draws") {
    // Same drawn == hit invariant the move gizmo's patch case pins, for the
    // segment bounds that moved when the centre box took over the middle.
    const GizmoFrame f = make_scale_frame();
    const simd_float3 eye = {0.8f, 0.3f, 8.0f};

    std::vector<LineVertex> out;
    append_scale_gizmo_handles(out, f, GizmoHandle::None, eye);
    CHECK(out.size() == 42); // 3 shafts + 3 tips + 1 centre box, 6 verts each

    // Just inboard of the drawn shaft start belongs to the centre, not the axis.
    const simd_float3 inboard = f.origin + 0.5f * kScaleAxisInnerFrac * f.half_extent * f.u;
    CHECK(pick(f, eye, inboard, GizmoKind::Scale) == GizmoHandle::Uniform);
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
