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

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera, GizmoSlot slot) {
    GizmoFrame f;

    if (slot == GizmoSlot::Placement && node.snapped) {
        // The surface the detail was placed on. Fixed in world space, so
        // "pull it out along the normal" means the same thing from every
        // camera angle.
        f.origin = node.snap_point;
        f.n = node.snap_normal;
        tangent_basis(f.n, f.u, f.v);
        // Both readings of the grid coincide here: the tangent plane IS the
        // surface and IS the u-v drag plane.
        f.grid_normal = f.n;
    } else {
        // The node's own local axes. Right-handed by construction (a rotation
        // preserves X x Y == Z), and exactly world X/Y/Z while the node's
        // rotation is identity -- which is what makes this a no-op for every
        // scene built before rotation existed.
        f.origin = node.position;
        f.u = simd_act(node.rotation, simd_float3{1.0f, 0.0f, 0.0f});
        f.v = simd_act(node.rotation, simd_float3{0.0f, 1.0f, 0.0f});
        f.n = simd_act(node.rotation, simd_float3{0.0f, 0.0f, 1.0f});
        // Shape draws no grid, so its value is inert -- set to n rather than
        // left unset, so there is no "meaningless unless" state to reason
        // about. A free Placement node gets a world-horizontal reference plane
        // instead of the u-v plane; see GizmoFrame's header comment.
        f.grid_normal = (slot == GizmoSlot::Placement) ? simd_float3{0.0f, 1.0f, 0.0f} : f.n;
    }

    f.half_extent = gizmo_half_extent(f.origin, camera);
    return f;
}

bool gizmos_coalesce(const GizmoFrame& placement, const GizmoFrame& shape) {
    return simd_distance(placement.origin, shape.origin) <
           kGizmoCoalesceFrac * placement.half_extent;
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

// Shared axis hit-test for both slots. The band bounds and the tolerance are
// the only things that differ, so the "drawn = hit" segment definition lives
// here once rather than being spelled out per slot.
struct AxisPick { GizmoHandle handle; float dist; float t; };

std::optional<AxisPick> pick_axis_handle(const GizmoFrame& frame, const Ray& ray,
                                          float pts_to_world_at_unit_depth,
                                          float inner_frac, float outer_frac,
                                          float tolerance_pts) {
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
        // Positive half only (R3): the drawn axis runs within this slot's band,
        // so the pickable segment does too — drawn geometry = hit geometry.
        const float s_clamped = std::clamp(*s, inner_frac * he, outer_frac * he);
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

// Ray distance to the Shape gizmo's uniform centre handle, or nullopt when it
// is outside tolerance. Returned as a DISTANCE rather than a bool because
// pick_gizmos has to weigh it against a competing handle on the other gizmo,
// which it cannot do with a yes/no.
std::optional<float> uniform_pick_distance(const GizmoFrame& frame, const Ray& ray,
                                            float pts_to_world_at_unit_depth) {
    const float t = std::fmax(simd_dot(frame.origin - ray.origin, ray.dir), 0.0f);
    const float dist = simd_length(frame.origin - (ray.origin + t * ray.dir));
    const float tol = kUniformPickTolerancePts * simd_length(frame.origin - ray.origin) *
                      pts_to_world_at_unit_depth;
    return (dist <= tol) ? std::optional<float>{dist} : std::nullopt;
}

} // namespace

GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts, GizmoSlot slot) {
    const float he = frame.half_extent;
    const float pts_to_world_at_unit_depth = 2.0f * std::tan(fov_y_radians * 0.5f) / viewport_h_pts;

    if (slot == GizmoSlot::Shape) {
        // The uniform centre is tested FIRST and the axes start outboard of it
        // (kScaleAxisInnerFrac), so the two never contend: aiming at the middle
        // always means uniform scale, which is the only reading a user has.
        if (uniform_pick_distance(frame, ray, pts_to_world_at_unit_depth)) {
            return GizmoHandle::Uniform;
        }
        const std::optional<AxisPick> axis =
            pick_axis_handle(frame, ray, pts_to_world_at_unit_depth, kScaleAxisInnerFrac,
                             kScaleAxisOuterFrac, kScaleAxisPickTolerancePts);
        return axis ? axis->handle : GizmoHandle::None; // no plane handles on Shape
    }

    if (const std::optional<AxisPick> axis =
            pick_axis_handle(frame, ray, pts_to_world_at_unit_depth, 0.0f, kMoveAxisOuterFrac,
                             kAxisPickTolerancePts)) {
        return axis->handle;
    }

    // Rings and plane patches both sit off-axis, and a single ray can pass near
    // one and then the other -- the bands are disjoint in RADIUS from the
    // origin, which says nothing about what a ray meets on its way through. So
    // they are gathered as candidates and the NEAREST ALONG THE RAY wins, which
    // is what "I clicked the thing in front" means. (Axes still beat both
    // outright, unchanged: they are the thinnest targets on the gizmo.)
    struct OuterPick { GizmoHandle handle; float t; };
    std::optional<OuterPick> ring_hit;

    // Rings resolved among THEMSELVES by true 3D distance from the ray to the
    // ring's CIRCLE, not by where the ray crosses each ring's plane.
    // The difference is not academic: a ray aimed squarely at one ring crosses
    // the other two rings' planes as it travels, and can cross one of them at
    // very nearly ring radius -- so a plane-based test picks a ring the cursor
    // was nowhere near, and the drag then reads its angle in the wrong plane.
    // Measuring to the nearest point ON each circle is the same technique
    // pick_axis_handle uses against a segment, and it makes the grazing ring
    // lose by the margin it deserves.
    {
        struct RingPick { GizmoHandle handle; float dist; float t; };
        std::optional<RingPick> best_ring;
        const float radius = kRotateRingFrac * he;
        const struct { simd_float3 axis; GizmoHandle handle; } rings[] = {
            {frame.u, GizmoHandle::RingU},
            {frame.v, GizmoHandle::RingV},
            {frame.n, GizmoHandle::RingN},
        };
        for (const auto& ring : rings) {
            if (std::fabs(simd_dot(ray.dir, ring.axis)) < kRotateRingViewAlignMin) {
                continue; // edge-on: undraggable, so ungrabbable
            }
            const std::optional<simd_float3> hit = ray_plane(ray, frame.origin, ring.axis);
            if (!hit) {
                continue;
            }
            const simd_float3 radial = *hit - frame.origin;
            if (simd_length_squared(radial) < 1e-12f) {
                continue; // dead centre: no direction to project onto the circle
            }
            // Nearest point on the circle to where the ray met the plane, then
            // that point's distance to the ray -- forward-clamped, exactly as
            // the axis test does.
            const simd_float3 on_ring = frame.origin + radius * simd_normalize(radial);
            const float t = std::fmax(simd_dot(on_ring - ray.origin, ray.dir), 0.0f);
            const float dist = simd_length(on_ring - (ray.origin + t * ray.dir));
            const float tol = kRotateRingPickTolerancePts * simd_length(on_ring - ray.origin) *
                              pts_to_world_at_unit_depth;
            if (dist > tol) {
                continue;
            }
            if (!best_ring || dist < best_ring->dist) {
                best_ring = RingPick{ring.handle, dist, t};
            }
        }
        if (best_ring) {
            ring_hit = OuterPick{best_ring->handle, best_ring->t};
        }
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

    // Nearest of the two families along the ray.
    if (ring_hit && best_plane) {
        return (ring_hit->t <= best_plane->t) ? ring_hit->handle : best_plane->handle;
    }
    if (ring_hit) {
        return ring_hit->handle;
    }
    return best_plane ? best_plane->handle : GizmoHandle::None;
}

GizmoHit pick_gizmos(const GizmoFrame& placement, const GizmoFrame& shape, const Ray& ray,
                     float fov_y_radians, float viewport_h_pts) {
    const float pts_to_world_at_unit_depth = 2.0f * std::tan(fov_y_radians * 0.5f) / viewport_h_pts;

    // The uniform centre and Placement's move axes are weighed by DISTANCE
    // rather than by a fixed order, because the two gizmos do not share an
    // origin once the pair is split -- and a screen-space contest cannot be
    // settled by a rule about world-space radii. Look along the tether (lift a
    // detail off its surface, then orbit to face the lift) and the two origins
    // project onto each other while Placement's axes stay fully visible around
    // them; giving the centre unconditional priority there swallowed handles
    // that were drawn, lit on hover, and plainly under the cursor.
    const std::optional<float> uniform_dist =
        uniform_pick_distance(shape, ray, pts_to_world_at_unit_depth);
    const std::optional<AxisPick> move_axis =
        pick_axis_handle(placement, ray, pts_to_world_at_unit_depth, 0.0f, kMoveAxisOuterFrac,
                         kAxisPickTolerancePts);

    if (uniform_dist && (!move_axis || *uniform_dist <= move_axis->dist)) {
        return GizmoHit{GizmoSlot::Shape, GizmoHandle::Uniform};
    }
    if (move_axis) {
        return GizmoHit{GizmoSlot::Placement, move_axis->handle};
    }
    if (const std::optional<AxisPick> scale_axis =
            pick_axis_handle(shape, ray, pts_to_world_at_unit_depth, kScaleAxisInnerFrac,
                             kScaleAxisOuterFrac, kScaleAxisPickTolerancePts)) {
        return GizmoHit{GizmoSlot::Shape, scale_axis->handle};
    }
    // Whatever is left on Placement: a rotation ring or a plane patch, already
    // resolved against each other by nearest-along-the-ray. Its axes were
    // handled above, so an axis coming back here would mean the two paths
    // disagree -- hence the guard rather than a bare return.
    const GizmoHandle rest =
        pick_gizmo_handle(placement, ray, fov_y_radians, viewport_h_pts, GizmoSlot::Placement);
    if (rest != GizmoHandle::None && !gizmo_handle_is_axis(rest)) {
        return GizmoHit{GizmoSlot::Placement, rest};
    }
    return GizmoHit{GizmoSlot::Placement, GizmoHandle::None};
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

bool gizmo_handle_is_ring(GizmoHandle handle) {
    return handle == GizmoHandle::RingU || handle == GizmoHandle::RingV ||
           handle == GizmoHandle::RingN;
}

simd_float3 gizmo_ring_axis(const GizmoFrame& frame, GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::RingU: return frame.u;
        case GizmoHandle::RingV: return frame.v;
        default:                 return frame.n;
    }
}

std::optional<simd_float3> ring_drag_dir(const Ray& ray, const GizmoFrame& frame,
                                          GizmoHandle handle) {
    const simd_float3 axis = gizmo_ring_axis(frame, handle);
    if (std::fabs(simd_dot(ray.dir, axis)) < kRotateRingViewAlignMin) {
        return std::nullopt; // edge-on: the intersection runs away and the angle is noise
    }
    const std::optional<simd_float3> hit = ray_plane(ray, frame.origin, axis);
    if (!hit) {
        return std::nullopt;
    }
    const simd_float3 radial = *hit - frame.origin;
    // Only the DIRECTION matters, not the radius: the drag is an angle, so a
    // cursor that wanders off the ring's circumference still turns it. The
    // guard is against the one place direction is undefined -- the centre.
    if (simd_length_squared(radial) < 1e-12f) {
        return std::nullopt;
    }
    return simd_normalize(radial);
}

float signed_angle_about(simd_float3 from, simd_float3 to, simd_float3 axis) {
    return std::atan2(simd_dot(simd_cross(from, to), axis), simd_dot(from, to));
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
