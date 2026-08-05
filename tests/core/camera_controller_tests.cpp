#include <doctest.h>

#include "camera.h"
#include "camera_controller.h"

#include <cmath>

using namespace sq;

namespace {

constexpr float kPi = 3.14159265358979323846f;

Camera make_camera(simd_float3 eye, simd_float3 target) {
    Camera camera;
    camera.eye = eye;
    camera.target = target;
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f;
    camera.aspect = 1.6f;
    return camera;
}

} // namespace

TEST_CASE("from_camera(to_camera()) round-trips a generic orbit state") {
    const simd_float3 target = {1.0f, 2.0f, 3.0f};
    const float yaw = 0.7f, pitch = 0.3f, radius = 5.0f;

    // Independently-derived eye: same target+radius*direction(yaw,pitch)
    // formula from the brief, computed here rather than reused from the
    // implementation.
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const simd_float3 dir = {cp * sy, sp, cp * cy};
    const simd_float3 eye = target + radius * dir;

    const CameraController start = CameraController::from_camera(make_camera(eye, target));
    const Camera round = start.to_camera();
    const CameraController end = CameraController::from_camera(round);

    CHECK(end.target().x == doctest::Approx(target.x));
    CHECK(end.target().y == doctest::Approx(target.y));
    CHECK(end.target().z == doctest::Approx(target.z));
    CHECK(end.yaw() == doctest::Approx(yaw));
    CHECK(end.pitch() == doctest::Approx(pitch));
    CHECK(end.radius() == doctest::Approx(radius));
}

TEST_CASE("from_camera decomposes eye/target into yaw/pitch/radius") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    {
        const CameraController c =
            CameraController::from_camera(make_camera(simd_float3{0.0f, 0.0f, 5.0f}, origin));
        CHECK(c.yaw() == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(c.pitch() == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(c.radius() == doctest::Approx(5.0f));
    }
    {
        const CameraController c =
            CameraController::from_camera(make_camera(simd_float3{5.0f, 0.0f, 0.0f}, origin));
        CHECK(c.yaw() == doctest::Approx(kPi / 2.0f));
    }
    {
        const simd_float3 eye = {0.0f, 5.0f * std::sin(0.5f), 5.0f * std::cos(0.5f)};
        const CameraController c = CameraController::from_camera(make_camera(eye, origin));
        CHECK(c.pitch() == doctest::Approx(0.5f));
        CHECK(c.radius() == doctest::Approx(5.0f));
    }
}

TEST_CASE("orbit: dx>0 increases yaw, dy<0 raises pitch, zero delta is a no-op") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, origin); // yaw 0, pitch 0

    CameraController pitch_up = CameraController::from_camera(cam);
    CHECK(pitch_up.orbit(0.0f, -100.0f));
    CHECK(pitch_up.pitch() == doctest::Approx(0.5f));
    CHECK(pitch_up.yaw() == doctest::Approx(0.0f));

    CameraController yaw_right = CameraController::from_camera(cam);
    CHECK(yaw_right.orbit(100.0f, 0.0f));
    CHECK(yaw_right.yaw() == doctest::Approx(0.5f));

    CameraController noop = CameraController::from_camera(cam);
    CHECK_FALSE(noop.orbit(0.0f, 0.0f));
}

TEST_CASE("orbit clamps pitch at kPitchLimit exactly, with no NaN, and further push is a no-op") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    CameraController c = CameraController::from_camera(make_camera(simd_float3{0.0f, 0.0f, 5.0f}, origin));

    CHECK(c.orbit(0.0f, -10000.0f));
    CHECK(c.pitch() == CameraController::kPitchLimit);

    const Camera result = c.to_camera();
    CHECK(std::isfinite(result.eye.x));
    CHECK(std::isfinite(result.eye.y));
    CHECK(std::isfinite(result.eye.z));

    CHECK_FALSE(c.orbit(0.0f, -1.0f));
}

TEST_CASE("zoom scales radius exponentially and is a no-op for zero delta") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    CameraController c = CameraController::from_camera(make_camera(simd_float3{0.0f, 0.0f, 5.0f}, origin));

    CHECK(c.zoom(1.0f));
    CHECK(c.radius() == doctest::Approx(5.0f * std::exp(-0.5f)));

    CHECK(c.zoom(-1.0f));
    CHECK(c.radius() == doctest::Approx(5.0f));

    CHECK_FALSE(c.zoom(0.0f));
}

TEST_CASE("zoom clamps at kRadiusMin and further zoom-in is a no-op") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    CameraController c = CameraController::from_camera(
        make_camera(simd_float3{0.0f, 0.0f, CameraController::kRadiusMin}, origin));

    CHECK_FALSE(c.zoom(1.0f));
    CHECK(c.radius() == CameraController::kRadiusMin);
}

TEST_CASE("zoom clamps at kRadiusMax and further zoom-out is a no-op") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    CameraController c = CameraController::from_camera(
        make_camera(simd_float3{0.0f, 0.0f, CameraController::kRadiusMax}, origin));

    CHECK_FALSE(c.zoom(-1.0f));
    CHECK(c.radius() == CameraController::kRadiusMax);
}

TEST_CASE("pan_view moves target in screen space, perpendicular to the view direction") {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, origin); // yaw 0, pitch 0, radius 5
    const float scale = 2.0f * 5.0f * std::tan(1.0472f * 0.5f) / 600.0f;

    {
        CameraController c = CameraController::from_camera(cam);
        const simd_float3 before = c.target();
        CHECK(c.pan_view(60.0f, 0.0f, 600.0f));
        const simd_float3 moved = c.target() - before;

        // right = {1,0,0} at yaw 0 / pitch 0 -> step = -right * 60 * scale.
        CHECK(moved.x == doctest::Approx(-60.0f * scale));
        CHECK(moved.y == doctest::Approx(0.0f));
        CHECK(moved.z == doctest::Approx(0.0f));

        const simd_float3 view_dir = simd_normalize(cam.target - cam.eye);
        CHECK(simd_dot(moved, view_dir) == doctest::Approx(0.0f).epsilon(1e-4));
    }
    {
        CameraController c = CameraController::from_camera(cam);
        const simd_float3 before = c.target();
        CHECK(c.pan_view(0.0f, 60.0f, 600.0f));
        const simd_float3 moved = c.target() - before;

        // up = {0,1,0} at yaw 0 / pitch 0 -> step = +up * 60 * scale.
        CHECK(moved.x == doctest::Approx(0.0f));
        CHECK(moved.y == doctest::Approx(60.0f * scale));
        CHECK(moved.z == doctest::Approx(0.0f));

        const simd_float3 view_dir = simd_normalize(cam.target - cam.eye);
        CHECK(simd_dot(moved, view_dir) == doctest::Approx(0.0f).epsilon(1e-4));
    }
    {
        CameraController c = CameraController::from_camera(cam);
        CHECK_FALSE(c.pan_view(0.0f, 0.0f, 600.0f));
    }
}

// --- cursor-anchored navigation --------------------------------------------

TEST_CASE("set_pivot_preserving_eye changes the orbit centre and nothing else") {
    // The invariant auto-pivot rests on. Not just the eye: the view direction,
    // target and radius must all hold, because re-deriving the pivot happens at
    // the START of every gesture and any change there is a visible jolt before
    // the user has dragged at all.
    //
    // An earlier draft folded the pivot into the look-at target. It preserved
    // the eye and still failed this: to_camera() derives the view direction
    // from the target, so an off-axis pivot swung the camera to centre it —
    // and aiming at a feature is by definition aiming off-centre.
    const Camera cam = make_camera(simd_float3{4.0f, 3.0f, 6.0f}, simd_float3{0.0f, 0.5f, 0.0f});

    const simd_float3 pivots[] = {
        {0.0f, 0.0f, 0.0f},   {1.0f, 2.0f, -1.0f}, {3.9f, 2.9f, 5.0f},
        {-4.0f, -2.0f, -3.0f}, {0.0f, -3.0f, 5.9f}, {0.0f, 0.0f, -500.0f},
    };

    for (const simd_float3 pivot : pivots) {
        CAPTURE(pivot.x);
        CAPTURE(pivot.y);
        CAPTURE(pivot.z);
        CameraController c = CameraController::from_camera(cam);
        const Camera before = c.to_camera();

        REQUIRE(c.set_pivot_preserving_eye(pivot));

        const Camera after = c.to_camera();
        CHECK(after.eye.x == doctest::Approx(before.eye.x));
        CHECK(after.eye.y == doctest::Approx(before.eye.y));
        CHECK(after.eye.z == doctest::Approx(before.eye.z));
        // The view direction, which is what the old conflated model broke.
        const simd_float3 f0 = simd_normalize(before.target - before.eye);
        const simd_float3 f1 = simd_normalize(after.target - after.eye);
        CHECK(simd_dot(f0, f1) == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(c.radius() == doctest::Approx(CameraController::from_camera(cam).radius()));

        // And the pivot really did take.
        CHECK(c.pivot().x == doctest::Approx(pivot.x));
        CHECK(c.pivot().y == doctest::Approx(pivot.y));
        CHECK(c.pivot().z == doctest::Approx(pivot.z));
    }
}

TEST_CASE("set_pivot_preserving_eye refuses only a non-finite pivot") {
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, simd_float3{0.0f, 0.0f, 0.0f});
    CameraController c = CameraController::from_camera(cam);
    const simd_float3 before = c.pivot();

    CHECK_FALSE(c.set_pivot_preserving_eye(simd_float3{std::nanf(""), 0.0f, 0.0f}));
    CHECK_FALSE(c.set_pivot_preserving_eye(simd_float3{0.0f, INFINITY, 0.0f}));
    CHECK(c.pivot().z == doctest::Approx(before.z)); // state untouched

    // Everything else is representable now that the pivot is separate state:
    // at the eye, straight below it, or far outside the radius clamp.
    CHECK(c.set_pivot_preserving_eye(cam.eye));
    CHECK(c.set_pivot_preserving_eye(simd_float3{0.0f, -3.0f, 5.0f}));
    CHECK(c.set_pivot_preserving_eye(simd_float3{0.0f, 0.0f, -500.0f}));
}

TEST_CASE("orbit around an off-axis pivot swings the rig without re-aiming the camera") {
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, simd_float3{0.0f, 0.0f, 0.0f});

    SUBCASE("with the pivot at the look-at target, behaviour is unchanged") {
        // The compatibility guarantee: the arm and view angle pairs are equal
        // by construction there, so the new path collapses onto the old one.
        CameraController c = CameraController::from_camera(cam);
        CHECK(c.orbit(100.0f, 0.0f));
        CHECK(c.yaw() == doctest::Approx(0.5f));
        CHECK(c.target().x == doctest::Approx(0.0f));
        CHECK(c.target().y == doctest::Approx(0.0f));
        CHECK(c.target().z == doctest::Approx(0.0f));
        CHECK(c.radius() == doctest::Approx(5.0f));
    }

    SUBCASE("an off-axis pivot keeps the eye on a sphere around THAT point") {
        CameraController c = CameraController::from_camera(cam);
        const simd_float3 pivot = {1.5f, 0.5f, 0.0f}; // off the view axis
        REQUIRE(c.set_pivot_preserving_eye(pivot));

        const simd_float3 eye0 = c.to_camera().eye;
        const float arm0 = simd_length(eye0 - pivot);
        const simd_float3 forward0 = simd_normalize(c.to_camera().target - eye0);

        REQUIRE(c.orbit(80.0f, -40.0f));

        const Camera after = c.to_camera();
        // Rigid: the distance to the pivot is preserved...
        CHECK(simd_length(after.eye - pivot) == doctest::Approx(arm0));
        // ...the eye actually moved...
        CHECK(simd_length(after.eye - eye0) > 0.1f);
        // ...and the view turned by the same angles the arm did, so the camera
        // never snapped its aim onto the pivot.
        const simd_float3 forward1 = simd_normalize(after.target - after.eye);
        CHECK(simd_dot(forward0, forward1) < 0.9999f); // it did rotate
        CHECK(c.yaw() == doctest::Approx(0.4f));       // 80 * kOrbitSens
    }
}

TEST_CASE("pan_world moves along world axes, not the view plane") {
    constexpr float h = 600.0f;
    constexpr float depth = 5.0f;
    const float scale = 2.0f * depth * std::tan(1.0472f * 0.5f) / h;

    SUBCASE("vertical drag at a pitched-down camera is PURE world Y") {
        // This is the case the old pan_view got wrong: with the camera pitched
        // 30 degrees down its up vector is (0, 0.866, -0.5), so a vertical drag
        // crept forward in z. Here z must stay exactly put.
        const float pitch = 0.5236f; // 30 degrees
        const simd_float3 eye = {0.0f, 5.0f * std::sin(pitch), 5.0f * std::cos(pitch)};
        CameraController c = CameraController::from_camera(make_camera(eye, simd_float3{0.0f, 0.0f, 0.0f}));
        REQUIRE(c.pitch() == doctest::Approx(pitch));

        const simd_float3 before = c.target();
        CHECK(c.pan_world(0.0f, 60.0f, depth, h));
        const simd_float3 moved = c.target() - before;

        CHECK(moved.x == doctest::Approx(0.0f));
        CHECK(moved.z == doctest::Approx(0.0f)); // the whole point
        CHECK(moved.y == doctest::Approx(60.0f * pan_vertical_gain(pitch) * scale));
    }

    SUBCASE("horizontal drag stays horizontal at every pitch") {
        for (const float pitch : {-1.4f, -0.7f, 0.0f, 0.7f, 1.4f}) {
            CAPTURE(pitch);
            const simd_float3 eye = {0.0f, 5.0f * std::sin(pitch), 5.0f * std::cos(pitch)};
            CameraController c = CameraController::from_camera(make_camera(eye, simd_float3{0.0f, 0.0f, 0.0f}));
            const simd_float3 before = c.target();
            CHECK(c.pan_world(60.0f, 0.0f, depth, h));
            // right = normalize(cross(f, {0,1,0})) has y == 0 for every pose in
            // this rig — the property that made horizontal pan world-locked
            // already, and left only the vertical axis to fix.
            CHECK((c.target() - before).y == doctest::Approx(0.0f));
        }
    }

    SUBCASE("zero drag and a zero-height viewport are no-ops") {
        CameraController c = CameraController::from_camera(
            make_camera(simd_float3{0.0f, 0.0f, 5.0f}, simd_float3{0.0f, 0.0f, 0.0f}));
        CHECK_FALSE(c.pan_world(0.0f, 0.0f, depth, h));
        CHECK_FALSE(c.pan_world(10.0f, 10.0f, depth, 0.0f));
        CHECK_FALSE(c.pan_world(10.0f, 10.0f, std::nanf(""), h));
    }
}

TEST_CASE("pan_up_blend and pan_vertical_gain retire together and stay bounded") {
    // Both must be exactly neutral at pitch 0 (pure world Y, no correction) and
    // again once the axis has become camera up (which needs no correction),
    // with nothing unbounded in between.
    CHECK(pan_up_blend(0.0f) == doctest::Approx(0.0f));
    CHECK(pan_vertical_gain(0.0f) == doctest::Approx(1.0f));

    // |sin| >= kPanBlendEndSin: fully blended to camera up.
    const float steep = std::asin(0.99f);
    CHECK(pan_up_blend(steep) == doctest::Approx(1.0f));
    CHECK(pan_vertical_gain(steep) == doctest::Approx(1.0f));

    // Below the blend's start the axis is pure world Y.
    CHECK(pan_up_blend(std::asin(0.7f)) == doctest::Approx(0.0f));

    float previous = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float pitch = -CameraController::kPitchLimit +
                            2.0f * CameraController::kPitchLimit * static_cast<float>(i) / 200.0f;
        CAPTURE(pitch);
        const float blend = pan_up_blend(pitch);
        const float gain = pan_vertical_gain(pitch);

        CHECK(blend >= 0.0f);
        CHECK(blend <= 1.0f);
        CHECK(std::isfinite(gain));
        CHECK(gain >= 1.0f);
        // gain = mix(raw, 1, blend) with raw <= 1/kPanGainCosFloor, so the
        // provable bound is the reciprocal of the floor — no pitch can make
        // the vertical drag explode.
        CHECK(gain <= doctest::Approx(1.0f / kPanGainCosFloor));

        // Symmetric in pitch: looking up and down correct identically.
        CHECK(pan_up_blend(-pitch) == doctest::Approx(blend));
        previous = gain;
    }
    CHECK(previous > 0.0f); // loop actually ran
}

TEST_CASE("dolly_toward keeps the focus point on the same pixel") {
    // The exactness claim of zoom-to-cursor, asserted as the property itself
    // rather than as the algebra behind it: whatever was under the cursor is
    // still under the cursor afterwards.
    constexpr float w = 800.0f, h = 500.0f;
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, simd_float3{0.0f, 0.0f, 0.0f});
    const simd_float3 focus = {0.5f, 0.3f, 0.0f}; // deliberately OFF the view axis

    const ViewPoint before = cam.project(focus, w, h);
    REQUIRE(before.visible);

    for (const float factor : {0.25f, 0.5f, 0.9f, 1.5f, 3.0f}) {
        CAPTURE(factor);
        CameraController c = CameraController::from_camera(cam);
        const float radius_before = c.radius();
        const float yaw_before = c.yaw();
        const float pitch_before = c.pitch();

        REQUIRE(c.dolly_toward(focus, factor));

        const ViewPoint after = c.to_camera().project(focus, w, h);
        CHECK(after.visible);
        CHECK(after.x == doctest::Approx(before.x).epsilon(1e-3));
        CHECK(after.y == doctest::Approx(before.y).epsilon(1e-3));

        // Rigid translation: orientation and radius are untouched by design.
        CHECK(c.radius() == doctest::Approx(radius_before));
        CHECK(c.yaw() == doctest::Approx(yaw_before));
        CHECK(c.pitch() == doctest::Approx(pitch_before));

        // And it actually moved the right way.
        const float distance = simd_length(c.to_camera().eye - focus);
        const float expected = simd_length(cam.eye - focus) * factor;
        CHECK(distance == doctest::Approx(expected));
    }
}

TEST_CASE("dolly_toward clamps on focus distance and refuses degenerate input") {
    const Camera cam = make_camera(simd_float3{0.0f, 0.0f, 5.0f}, simd_float3{0.0f, 0.0f, 0.0f});
    const simd_float3 focus = {0.0f, 0.0f, 0.0f};

    SUBCASE("zooming in saturates at kFocusDistMin, and further zoom is a no-op") {
        CameraController c = CameraController::from_camera(cam);
        CHECK(c.dolly_toward(focus, 1e-6f));
        CHECK(simd_length(c.to_camera().eye - focus) == doctest::Approx(CameraController::kFocusDistMin));
        CHECK_FALSE(c.dolly_toward(focus, 1e-6f));
    }

    SUBCASE("zooming out saturates at kRadiusMax") {
        CameraController c = CameraController::from_camera(cam);
        CHECK(c.dolly_toward(focus, 1e6f));
        CHECK(simd_length(c.to_camera().eye - focus) == doctest::Approx(CameraController::kRadiusMax));
        CHECK_FALSE(c.dolly_toward(focus, 1e6f));
    }

    SUBCASE("non-finite or non-positive input is refused") {
        CameraController c = CameraController::from_camera(cam);
        const float nan = std::nanf("");
        CHECK_FALSE(c.dolly_toward(simd_float3{nan, 0.0f, 0.0f}, 0.5f));
        CHECK_FALSE(c.dolly_toward(focus, nan));
        CHECK_FALSE(c.dolly_toward(focus, 0.0f));
        CHECK_FALSE(c.dolly_toward(focus, -1.0f));
        CHECK_FALSE(c.dolly_toward(focus, 1.0f)); // exactly no change
        CHECK_FALSE(c.dolly_toward(cam.eye, 0.5f)); // focus at the eye
    }
}

TEST_CASE("dolly_points_for_magnification matches the pinch and drag rates") {
    // A pinch and the equivalent drag must produce the same zoom, which is the
    // only reason both sensitivities live on the controller.
    const float points = CameraController::dolly_points_for_magnification(1.0f);
    CHECK(points == doctest::Approx(CameraController::kZoomSens / CameraController::kDollySens));
    CHECK(std::exp(-points * CameraController::kDollySens) ==
          doctest::Approx(std::exp(-1.0f * CameraController::kZoomSens)));
    CHECK(CameraController::dolly_points_for_magnification(0.0f) == doctest::Approx(0.0f));
    CHECK(CameraController::dolly_points_for_magnification(-2.0f) == doctest::Approx(-2.0f * points));
}

TEST_CASE("from_camera decomposes a degenerate eye == target without NaN") {
    const simd_float3 p = {1.0f, 2.0f, 3.0f};
    const CameraController c = CameraController::from_camera(make_camera(p, p));

    CHECK(c.radius() == doctest::Approx(CameraController::kRadiusEps));
    CHECK(c.yaw() == doctest::Approx(0.0f));
    CHECK(c.pitch() == doctest::Approx(0.0f));

    const Camera round = c.to_camera();
    CHECK(std::isfinite(round.eye.x));
    CHECK(std::isfinite(round.eye.y));
    CHECK(std::isfinite(round.eye.z));
}
