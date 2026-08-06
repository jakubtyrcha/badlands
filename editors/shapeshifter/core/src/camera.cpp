#include "camera.h"

#include <cmath>

namespace sq {

namespace {

// Right-handed look-at basis: f points from eye to target, s is "right", u is
// the reconstructed "up". Shared by view_proj() and ray_through_view_point()
// so the ray basis always matches what the projection actually shows.
struct Basis {
    simd_float3 f;
    simd_float3 s;
    simd_float3 u;
};

Basis look_at_basis(const Camera& camera) {
    simd_float3 f = simd_normalize(camera.target - camera.eye);
    simd_float3 s = simd_normalize(simd_cross(f, camera.up));
    simd_float3 u = simd_cross(s, f);
    return {f, s, u};
}

// REVERSED-Z: the near plane maps to depth 1 and the far plane to 0, the
// opposite of the usual Metal [0,1] mapping.
//
// Two reasons, one of them binding. It is an invariant of the engine's RHI
// (src/engine/rhi) -- depth clears to 0 and opaque geometry compares
// GreaterEqual there -- and this renderer is being ported onto it, so the
// convention is not ours to choose. It is also simply better: float32 packs its
// exponent near zero, and reversing puts that precision at the far plane where
// 1/z has thrown it away, instead of at the near plane where 1/z already
// concentrates it.
//
// Derivation, so the columns are checkable rather than magic. With clip.w = -z
// (the -1 in column 2), we need clip.z = a*z + b such that z = -near maps to
// depth 1 and z = -far maps to 0:
//     (-a*near + b) / near = 1  and  (-a*far + b) / far = 0
//     => b = a*far, and a*(far - near) = near
//     => a = near / (far - near), b = near*far / (far - near)
simd_float4x4 perspective_matrix(float fov_y_radians, float aspect, float near, float far) {
    const float h = 1.0f / std::tan(fov_y_radians * 0.5f);
    simd_float4x4 m;
    m.columns[0] = (simd_float4){h / aspect, 0.0f, 0.0f, 0.0f};
    m.columns[1] = (simd_float4){0.0f, h, 0.0f, 0.0f};
    m.columns[2] = (simd_float4){0.0f, 0.0f, near / (far - near), -1.0f};
    m.columns[3] = (simd_float4){0.0f, 0.0f, near * far / (far - near), 0.0f};
    return m;
}

simd_float4x4 view_matrix(const Camera& camera, const Basis& basis) {
    const simd_float3& s = basis.s;
    const simd_float3& u = basis.u;
    const simd_float3& f = basis.f;
    simd_float4x4 m;
    m.columns[0] = (simd_float4){s.x, u.x, -f.x, 0.0f};
    m.columns[1] = (simd_float4){s.y, u.y, -f.y, 0.0f};
    m.columns[2] = (simd_float4){s.z, u.z, -f.z, 0.0f};
    m.columns[3] = (simd_float4){
        -simd_dot(camera.eye, s), -simd_dot(camera.eye, u), simd_dot(camera.eye, f), 1.0f};
    return m;
}

} // namespace

simd_float4x4 Camera::view_proj() const {
    const Basis basis = look_at_basis(*this);
    const simd_float4x4 p = perspective_matrix(fov_y_radians, aspect, kNear, kFar);
    const simd_float4x4 v = view_matrix(*this, basis);
    return simd_mul(p, v);
}

Ray Camera::ray_through_view_point(float x, float y, float w_pts, float h_pts) const {
    const Basis basis = look_at_basis(*this);
    const float ndc_x = 2.0f * x / w_pts - 1.0f;
    const float ndc_y = 1.0f - 2.0f * y / h_pts;
    const float half_h = std::tan(fov_y_radians * 0.5f);
    const float half_w = half_h * aspect;
    const simd_float3 dir = simd_normalize(basis.f + ndc_x * half_w * basis.s + ndc_y * half_h * basis.u);
    return Ray{eye, dir};
}

ViewPoint Camera::project(simd_float3 world, float w_pts, float h_pts) const {
    const simd_float4 clip = simd_mul(view_proj(), (simd_float4){world.x, world.y, world.z, 1.0f});
    if (clip.w <= 0.0f) {
        return ViewPoint{0.0f, 0.0f, false};
    }
    const simd_float3 ndc = clip.xyz / clip.w;
    const float x_view = (ndc.x * 0.5f + 0.5f) * w_pts;
    const float y_view = (1.0f - (ndc.y * 0.5f + 0.5f)) * h_pts;
    return ViewPoint{x_view, y_view, true};
}

} // namespace sq
