#include <doctest.h>

#include "camera.h"

#include <cmath>
#include <utility>
#include <vector>

using namespace sq;

namespace {

// Independent re-derivation of the brief's pinned column formulas, so this
// test actually pins Camera::view_proj()'s output rather than echoing its
// implementation back at itself.
simd_float4x4 reference_projection(float fov_y_radians, float aspect, float near, float far) {
    const float h = 1.0f / std::tan(fov_y_radians * 0.5f);
    simd_float4x4 m;
    m.columns[0] = (simd_float4){h / aspect, 0.0f, 0.0f, 0.0f};
    m.columns[1] = (simd_float4){0.0f, h, 0.0f, 0.0f};
    m.columns[2] = (simd_float4){0.0f, 0.0f, -far / (far - near), -1.0f};
    m.columns[3] = (simd_float4){0.0f, 0.0f, -near * far / (far - near), 0.0f};
    return m;
}

simd_float4x4 reference_view(simd_float3 eye, simd_float3 target, simd_float3 up) {
    const simd_float3 f = simd_normalize(target - eye);
    const simd_float3 s = simd_normalize(simd_cross(f, up));
    const simd_float3 u = simd_cross(s, f);
    simd_float4x4 m;
    m.columns[0] = (simd_float4){s.x, u.x, -f.x, 0.0f};
    m.columns[1] = (simd_float4){s.y, u.y, -f.y, 0.0f};
    m.columns[2] = (simd_float4){s.z, u.z, -f.z, 0.0f};
    m.columns[3] = (simd_float4){-simd_dot(eye, s), -simd_dot(eye, u), simd_dot(eye, f), 1.0f};
    return m;
}

Camera make_camera(float aspect) {
    Camera camera;
    camera.eye = {4.0f, 3.0f, 6.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f;
    camera.aspect = aspect;
    return camera;
}

} // namespace

TEST_CASE("Camera::view_proj matches the pinned P*V formula") {
    const Camera camera = make_camera(1.6f);
    const simd_float4x4 expected = simd_mul(
        reference_projection(camera.fov_y_radians, camera.aspect, Camera::kNear, Camera::kFar),
        reference_view(camera.eye, camera.target, camera.up));
    const simd_float4x4 actual = camera.view_proj();

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            CAPTURE(col);
            CAPTURE(row);
            CHECK(actual.columns[col][row] == doctest::Approx(expected.columns[col][row]).epsilon(1e-5));
        }
    }
}

TEST_CASE("ray_through_view_point through the viewport center points at the target") {
    const Camera camera = make_camera(1.6f);
    const float w = 800.0f, h = 600.0f;

    const Ray ray = camera.ray_through_view_point(w * 0.5f, h * 0.5f, w, h);
    const simd_float3 expected_dir = simd_normalize(camera.target - camera.eye);

    CHECK(ray.origin.x == doctest::Approx(camera.eye.x));
    CHECK(ray.origin.y == doctest::Approx(camera.eye.y));
    CHECK(ray.origin.z == doctest::Approx(camera.eye.z));
    CHECK(ray.dir.x == doctest::Approx(expected_dir.x));
    CHECK(ray.dir.y == doctest::Approx(expected_dir.y));
    CHECK(ray.dir.z == doctest::Approx(expected_dir.z));
}

TEST_CASE("project round-trips ray_through_view_point for several view points") {
    const Camera camera = make_camera(1.6f);
    const float w = 800.0f, h = 600.0f;
    const float t = 5.0f;

    const std::vector<std::pair<float, float>> view_points = {
        {w * 0.5f, h * 0.5f},
        {0.25f * w, 0.7f * h},
        {0.9f * w, 0.1f * h},
    };

    for (const auto& [x, y] : view_points) {
        CAPTURE(x);
        CAPTURE(y);
        const Ray ray = camera.ray_through_view_point(x, y, w, h);
        const simd_float3 world = camera.eye + t * ray.dir;
        const ViewPoint p2 = camera.project(world, w, h);

        CHECK(p2.visible);
        CHECK(p2.x == doctest::Approx(x).epsilon(1e-3));
        CHECK(p2.y == doctest::Approx(y).epsilon(1e-3));
    }
}

TEST_CASE("project reports a world point behind the camera as not visible") {
    const Camera camera = make_camera(1.6f);
    const simd_float3 behind = camera.eye + 2.0f * (camera.eye - camera.target);

    const ViewPoint p = camera.project(behind, 800.0f, 600.0f);

    CHECK_FALSE(p.visible);
}
