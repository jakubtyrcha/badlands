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
