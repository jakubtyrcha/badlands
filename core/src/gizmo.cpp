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

namespace {

// Screen-constant apparent size: half_extent as a fraction of viewport height
// at the frame origin's depth. Shared so both kinds cannot drift apart.
float gizmo_half_extent(simd_float3 origin, const Camera& camera) {
    return kGizmoScreenFraction * simd_length(origin - camera.eye) * 2.0f *
           std::tan(camera.fov_y_radians * 0.5f);
}

} // namespace

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera, GizmoKind kind) {
    GizmoFrame f;

    if (kind == GizmoKind::Scale) {
        // The node's own local axes, centred on the node. See GizmoFrame's
        // header comment for why a camera-facing basis would be wrong here.
        // Right-handed by construction: X x Y == Z.
        f.origin = node.position;
        f.u = simd_float3{1.0f, 0.0f, 0.0f};
        f.v = simd_float3{0.0f, 1.0f, 0.0f};
        f.n = simd_float3{0.0f, 0.0f, 1.0f};
    } else {
        const simd_float3 forward = simd_normalize(camera.target - camera.eye);
        const DragPlane dp = drag_plane_for_node(node, forward);
        f.origin = dp.point;
        f.n = dp.normal;
        tangent_basis(f.n, f.u, f.v);
    }

    f.half_extent = gizmo_half_extent(f.origin, camera);
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

namespace {

// Shared axis hit-test for both gizmo kinds. `inner_frac`/`tolerance_pts` are
// the only things that differ, so the "drawn = hit" segment definition lives
// here once rather than being spelled out per kind.
struct AxisPick { GizmoHandle handle; float dist; float t; };

std::optional<AxisPick> pick_axis_handle(const GizmoFrame& frame, const Ray& ray,
                                          float pts_to_world_at_unit_depth,
                                          float inner_frac, float tolerance_pts) {
    const float he = frame.half_extent;
    std::optional<AxisPick> best;
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
        // Positive half only (R3): the drawn axis runs inner_frac*he..+he, so
        // the pickable segment does too — drawn geometry = hit geometry.
        const float s_clamped = std::clamp(*s, inner_frac * he, he);
        const simd_float3 p_axis = frame.origin + s_clamped * axis.dir;
        // Forward-clamp the ray parameter: geometry behind the ray origin can
        // only be "closest" at t = 0, where it is far outside tolerance.
        const float t = std::fmax(simd_dot(p_axis - ray.origin, ray.dir), 0.0f);
        const simd_float3 p_ray = ray.origin + t * ray.dir;
        const float dist = simd_length(p_axis - p_ray);
        const float tol = tolerance_pts * simd_length(p_axis - ray.origin) * pts_to_world_at_unit_depth;
        if (dist > tol) {
            continue;
        }
        // Smallest distance wins; ties (the origin, where all three axes meet)
        // fall to smaller t, then declaration order. "Tie" is epsilon-based:
        // at the origin the three distances agree only up to fp rounding, and
        // an exact == would hand the result to whichever axis the noise
        // happens to favor.
        constexpr float kTieEps = 1e-5f;
        if (!best || dist < best->dist - kTieEps ||
            (std::fabs(dist - best->dist) <= kTieEps && t < best->t - kTieEps)) {
            best = AxisPick{axis.handle, dist, t};
        }
    }
    return best;
}

} // namespace

GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts, GizmoKind kind) {
    const float he = frame.half_extent;
    const float pts_to_world_at_unit_depth = 2.0f * std::tan(fov_y_radians * 0.5f) / viewport_h_pts;

    if (kind == GizmoKind::Scale) {
        // The uniform centre is tested FIRST and the axes start outboard of it
        // (kScaleAxisInnerFrac), so the two never contend: aiming at the middle
        // always means uniform scale, which is the only reading a user has.
        const float t = std::fmax(simd_dot(frame.origin - ray.origin, ray.dir), 0.0f);
        const float dist = simd_length(frame.origin - (ray.origin + t * ray.dir));
        const float tol = kUniformPickTolerancePts * simd_length(frame.origin - ray.origin) *
                          pts_to_world_at_unit_depth;
        if (dist <= tol) {
            return GizmoHandle::Uniform;
        }
        const std::optional<AxisPick> axis = pick_axis_handle(
            frame, ray, pts_to_world_at_unit_depth, kScaleAxisInnerFrac, kScaleAxisPickTolerancePts);
        return axis ? axis->handle : GizmoHandle::None; // no plane handles on scale
    }

    if (const std::optional<AxisPick> axis =
            pick_axis_handle(frame, ray, pts_to_world_at_unit_depth, 0.0f, kAxisPickTolerancePts)) {
        return axis->handle;
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
        // Same constants append_move_gizmo_handles draws the patch from, so
        // the outline and its hit region cannot drift apart.
        if (x < kGizmoPatchInner * he || x > kGizmoPatchOuter * he ||
            y < kGizmoPatchInner * he || y > kGizmoPatchOuter * he) {
            continue;
        }
        const float t = simd_dot(*hit - ray.origin, ray.dir);
        if (!best_plane || t < best_plane->t) {
            best_plane = PlanePick{plane.handle, t};
        }
    }
    return best_plane ? best_plane->handle : GizmoHandle::None;
}

float pivot_marker_alpha(float seconds_since_activity) {
    // `!(x > hold)` rather than `x <= hold` so a NaN elapsed time (which no
    // caller should produce, but which would otherwise fall through to the
    // fade and yield a NaN alpha) resolves to fully visible instead.
    if (!(seconds_since_activity > kPivotHoldSeconds)) {
        return 1.0f;
    }
    const float t = (seconds_since_activity - kPivotHoldSeconds) / kPivotFadeSeconds;
    if (t >= 1.0f) {
        return 0.0f;
    }
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

bool gizmo_handle_is_axis(GizmoHandle handle) {
    // Uniform is deliberately NOT an axis: it drives a screen-space vertical
    // drag, not an axis-parameter solve, so anything branching on "is this an
    // axis handle?" must route it elsewhere.
    return handle == GizmoHandle::AxisU || handle == GizmoHandle::AxisV || handle == GizmoHandle::AxisN;
}

std::optional<float> scale_axis_param(const Ray& ray, const GizmoFrame& frame, GizmoHandle handle) {
    const std::optional<float> s = ray_axis_param(ray, frame.origin, gizmo_axis_dir(frame, handle));
    if (!s) {
        return std::nullopt;
    }
    // The floor is what makes factor = s_now / s_start well-behaved: applied to
    // BOTH the captured start and every update, it keeps the ratio positive and
    // finite, equal to 1 at mouse-down, and merely saturating (rather than
    // sign-flipping) when dragged inboard through the origin.
    return std::fmax(*s, kScaleAxisMinGrabFrac * frame.half_extent);
}

int gizmo_scale_axis_index(GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::AxisU: return 0;
        case GizmoHandle::AxisV: return 1;
        case GizmoHandle::AxisN: return 2;
        default:                 return -1;
    }
}

simd_float3 gizmo_axis_dir(const GizmoFrame& frame, GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::AxisU: return frame.u;
        case GizmoHandle::AxisV: return frame.v;
        default:                 return frame.n;
    }
}

simd_float3 gizmo_plane_normal(const GizmoFrame& frame, GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::PlaneUV: return frame.n;
        case GizmoHandle::PlaneUN: return frame.v;
        default:                   return frame.u;
    }
}

} // namespace sq
