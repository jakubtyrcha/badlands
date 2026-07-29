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

// --- Independently-derived literals -----------------------------------------
//
// The tests above re-derive the brief's formulas *inside this file*, so a
// transcription error shared between camera.cpp and this file's reference_*()
// helpers (or a self-consistent-but-wrong pair of round-trip conventions,
// e.g. an un-flipped ndc_y paired with an un-flipped y_view) would not be
// caught. These two cases instead pin literal numbers computed completely
// outside this codebase, by a standalone python3/numpy script implementing
// the same brief formulas from scratch (eye {4,3,6}, target {0,0.5,0},
// up {0,1,0}, fov_y_radians 1.0472, aspect 1.6, near 0.1, far 100):
//
//   python3 -c "
//   import numpy as np
//   eye=np.array([4.,3.,6.]); target=np.array([0.,.5,0.]); up=np.array([0.,1.,0.])
//   fov_y=1.0472; aspect=1.6; near,far=0.1,100.0
//   f=(target-eye)/np.linalg.norm(target-eye)
//   s=np.cross(f,up); s=s/np.linalg.norm(s)
//   u=np.cross(s,f)
//   h=1.0/np.tan(fov_y/2.0)
//   P=np.zeros((4,4)); P[:,0]=[h/aspect,0,0,0]; P[:,1]=[0,h,0,0]
//   P[:,2]=[0,0,-far/(far-near),-1]; P[:,3]=[0,0,-near*far/(far-near),0]
//   V=np.zeros((4,4)); V[:,0]=[s[0],u[0],-f[0],0]; V[:,1]=[s[1],u[1],-f[1],0]
//   V[:,2]=[s[2],u[2],-f[2],0]; V[:,3]=[-np.dot(eye,s),-np.dot(eye,u),np.dot(eye,f),1]
//   VP=P@V
//   print(VP)
//   def project(world,w,h):
//       clip=VP@np.array([*world,1.0]); ndc=clip[:3]/clip[3]
//       return (ndc[0]*0.5+0.5)*w, (1.0-(ndc[1]*0.5+0.5))*h
//   print(project((0,2.5,0),800,600))
//   print(project((0,-1.5,0),800,600))
//   print(project((2,0.5,0),800,600))
//   "
//
// (col3[0] printed as 4.8e-16 — algebraically exact 0, since dot(eye,s) is
// exactly 0 for this eye/up/target; treated as 0.0 here.)
TEST_CASE("Camera::view_proj matches independently computed (python/numpy) literals") {
    const Camera camera = make_camera(1.6f);
    const simd_float4x4 actual = camera.view_proj();

    const simd_float4x4 expected = {(simd_float4){
                                         0.9007183182495269f,
                                         -0.31470943456219075f,
                                         -0.5246220477120467f,
                                         -0.5240974256643347f,
                                     },
                                     (simd_float4){
                                         0.0f,
                                         1.6364890597233914f,
                                         -0.3278887798200292f,
                                         -0.3275608910402092f,
                                     },
                                     (simd_float4){
                                         -0.600478878833018f,
                                         -0.472064151843286f,
                                         -0.78693307156807f,
                                         -0.7861461384965021f,
                                     },
                                     (simd_float4){
                                         0.0f,
                                         -0.8182445298616959f,
                                         7.703652859616595f,
                                         7.795949206756979f,
                                     }};

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            CAPTURE(col);
            CAPTURE(row);
            CHECK(actual.columns[col][row] == doctest::Approx(expected.columns[col][row]).epsilon(1e-4));
        }
    }
}

TEST_CASE("Camera::project matches independently computed (python/numpy) literals, "
          "including a concrete y-flip check") {
    const Camera camera = make_camera(1.6f);
    const float w = 800.0f, h = 600.0f;

    // World-space point above eye/target's shared plane -> must land in the
    // upper half of the view (y_view < h/2 with top-left-origin, y-down
    // view points). If the ndc_y / y_view flip convention regresses, this
    // point would instead land in the lower half and the literal check
    // below would fail.
    const ViewPoint above = camera.project({0.0f, 2.5f, 0.0f}, w, h);
    CHECK(above.visible);
    CHECK(above.x == doctest::Approx(400.0f).epsilon(1e-4));
    CHECK(above.y == doctest::Approx(159.26804867913495f).epsilon(1e-4));
    CHECK(above.y < h * 0.5f); // the concrete y-flip-sensitive assertion

    // World-space point below -> lower half (y_view > h/2).
    const ViewPoint below = camera.project({0.0f, -1.5f, 0.0f}, w, h);
    CHECK(below.visible);
    CHECK(below.x == doctest::Approx(400.0f).epsilon(1e-4));
    CHECK(below.y == doctest::Approx(418.4818404400959f).epsilon(1e-4));
    CHECK(below.y > h * 0.5f); // the concrete y-flip-sensitive assertion

    // An off-center point exercising both x and y together.
    const ViewPoint side = camera.project({2.0f, 0.5f, 0.0f}, w, h);
    CHECK(side.visible);
    CHECK(side.x == doctest::Approx(509.44372873580664f).epsilon(1e-4));
    CHECK(side.y == doctest::Approx(328.67958824277105f).epsilon(1e-4));
}
