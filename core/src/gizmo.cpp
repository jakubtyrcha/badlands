#include "gizmo.h"

#include <algorithm>
#include <cmath>

#include "picking.h"
#include "scene.h"

namespace sq {

void tangent_basis(simd_float3 n, simd_float3& u, simd_float3& v) {
    if (simd_length_squared(n) < 1e-8f) {
        n = simd_float3{0.0f, 1.0f, 0.0f};
    }
    const simd_float3 ref = (std::fabs(n.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f}
                                                     : simd_float3{1.0f, 0.0f, 0.0f};
    u = simd_normalize(simd_cross(n, ref));
    v = simd_cross(n, u);
}

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera) {
    const simd_float3 forward = simd_normalize(camera.target - camera.eye);
    const DragPlane dp = drag_plane_for_node(node, forward);

    GizmoFrame f;
    f.origin = dp.point;
    f.n = dp.normal;
    tangent_basis(f.n, f.u, f.v);
    f.half_extent = kGizmoScreenFraction * simd_length(dp.point - camera.eye) *
                    2.0f * std::tan(camera.fov_y_radians * 0.5f);
    return f;
}

std::optional<float> ray_axis_param(const Ray& ray, simd_float3 origin, simd_float3 axis_dir) {
    // Standard two-line closest-point system; b = cos of the line angle, so
    // the denominator is sin^2 — near-parallel means the system is singular.
    const float b = simd_dot(ray.dir, axis_dir);
    const float denom = 1.0f - b * b;
    if (denom < 1e-6f) {
        return std::nullopt;
    }
    const simd_float3 w0 = ray.origin - origin;
    const float d0 = simd_dot(ray.dir, w0);
    const float e  = simd_dot(axis_dir, w0);
    return (e - b * d0) / denom;
}

GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts) {
    const float he = frame.half_extent;
    const float pts_to_world_at_unit_depth = 2.0f * std::tan(fov_y_radians * 0.5f) / viewport_h_pts;

    struct AxisPick { GizmoHandle handle; float dist; float t; };
    std::optional<AxisPick> best_axis;
    const struct { simd_float3 dir; GizmoHandle handle; } axes[] = {
        {frame.u, GizmoHandle::AxisU},
        {frame.v, GizmoHandle::AxisV},
        {frame.n, GizmoHandle::AxisN},
    };
    for (const auto& axis : axes) {
        if (std::fabs(simd_dot(ray.dir, axis.dir)) > kAxisViewAlignLimit) {
            continue; // end-on: undraggable, so ungrabbable (see kAxisViewAlignLimit)
        }
        const std::optional<float> s = ray_axis_param(ray, frame.origin, axis.dir);
        if (!s) {
            continue;
        }
        const float s_clamped = std::clamp(*s, -he, he);
        const simd_float3 p_axis = frame.origin + s_clamped * axis.dir;
        // Forward-clamp the ray parameter: geometry behind the ray origin can
        // only be "closest" at t = 0, where it is far outside tolerance.
        const float t = std::fmax(simd_dot(p_axis - ray.origin, ray.dir), 0.0f);
        const simd_float3 p_ray = ray.origin + t * ray.dir;
        const float dist = simd_length(p_axis - p_ray);
        const float tol = kAxisPickTolerancePts * simd_length(p_axis - ray.origin) * pts_to_world_at_unit_depth;
        if (dist > tol) {
            continue;
        }
        // Smallest distance wins; ties (the origin, where all three axes meet)
        // fall to smaller t, then declaration order. "Tie" is epsilon-based:
        // at the origin the three distances agree only up to fp rounding, and
        // an exact == would hand the result to whichever axis the noise
        // happens to favor.
        constexpr float kTieEps = 1e-5f;
        if (!best_axis || dist < best_axis->dist - kTieEps ||
            (std::fabs(dist - best_axis->dist) <= kTieEps && t < best_axis->t - kTieEps)) {
            best_axis = AxisPick{axis.handle, dist, t};
        }
    }
    if (best_axis) {
        return best_axis->handle;
    }

    struct PlanePick { GizmoHandle handle; float t; };
    std::optional<PlanePick> best_plane;
    const struct { simd_float3 e1, e2, normal; GizmoHandle handle; } planes[] = {
        {frame.u, frame.v, frame.n, GizmoHandle::PlaneUV},
        {frame.u, frame.n, frame.v, GizmoHandle::PlaneUN},
        {frame.v, frame.n, frame.u, GizmoHandle::PlaneVN},
    };
    for (const auto& plane : planes) {
        const std::optional<simd_float3> hit = ray_plane(ray, frame.origin, plane.normal);
        if (!hit) {
            continue;
        }
        const float x = simd_dot(*hit - frame.origin, plane.e1);
        const float y = simd_dot(*hit - frame.origin, plane.e2);
        if (x < 0.3f * he || x > 0.6f * he || y < 0.3f * he || y > 0.6f * he) {
            continue;
        }
        const float t = simd_dot(*hit - ray.origin, ray.dir);
        if (!best_plane || t < best_plane->t) {
            best_plane = PlanePick{plane.handle, t};
        }
    }
    return best_plane ? best_plane->handle : GizmoHandle::None;
}

} // namespace sq
