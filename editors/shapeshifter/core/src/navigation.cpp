#include "navigation.h"

#include <cmath>
#include <optional>

#include "math_util.h" // is_finite3
#include "picking.h"
#include "scene.h"

namespace sq {

namespace {

// The ground plate's plane (shaders/ground_grid.h draws it at y=0).
constexpr simd_float3 kGroundPoint  = {0.0f, 0.0f, 0.0f};
constexpr simd_float3 kGroundNormal = {0.0f, 1.0f, 0.0f};

} // namespace

FocusPoint resolve_focus(const SceneDocument& doc, const Ray& ray, simd_float3 fallback_target) {
    // 1. The model. This is the rung that makes "point at the nostril and
    //    drag" work, so everything below is a graceful degradation, not a
    //    peer.
    if (const std::optional<PickHit> hit = raycast_scene(doc, ray)) {
        if (is_finite3(hit->hit.point)) {
            return FocusPoint{hit->hit.point, FocusSource::Scene};
        }
    }

    // 2. The ground plate. ray_plane already rejects parallel rays and
    //    intersections behind the origin; the distance bound here is the extra
    //    guard for rays that merely *graze* the plane — those pass the
    //    parallel test but land absurdly far away. The finiteness check catches
    //    the NaN that a camera sitting exactly at y=0 produces (dir.y == 0
    //    with the plane through the origin), which no comparison would: IEEE
    //    comparisons against NaN are all false, so `t < bound` would not
    //    reject it.
    if (const std::optional<simd_float3> ground = ray_plane(ray, kGroundPoint, kGroundNormal)) {
        const float t = simd_dot(*ground - ray.origin, ray.dir);
        if (is_finite3(*ground) && t < kFocusGroundMaxT) {
            return FocusPoint{*ground, FocusSource::Ground};
        }
    }

    // 3. The current target's depth, sampled under the cursor. Using the ray's
    //    own direction as the plane normal makes the denominator exactly -1,
    //    so this can never fail for being parallel — only for a target at or
    //    behind the eye, which rung 4 mops up.
    if (const std::optional<simd_float3> plane = ray_plane(ray, fallback_target, -ray.dir)) {
        if (is_finite3(*plane)) {
            return FocusPoint{*plane, FocusSource::TargetPlane};
        }
    }

    // 4. The target itself. Reported as TargetPlane because callers only ever
    //    branch on Scene vs not-Scene, and inventing a fourth case would be a
    //    distinction with no consumer.
    return FocusPoint{is_finite3(fallback_target) ? fallback_target : kGroundPoint,
                      FocusSource::TargetPlane};
}

float frame_radius_for_bound(float bound, float fov_y_radians) {
    const float half_fov = 0.5f * fov_y_radians;
    const float sin_half = std::sin(half_fov);
    if (!std::isfinite(bound) || !(sin_half > 1e-4f)) {
        return kMinFrameRadius;
    }
    return std::fmax(bound * kFrameMargin / sin_half, kMinFrameRadius);
}

float node_bounding_radius(const Node& node) {
    const simd_float3 half = 0.5f * simd_abs(node.scale);
    // The ellipsoid is the one shape that cannot reach its box's corners, so it
    // gets the tighter bound. Everything else can (a cube at its corners, a
    // pyramid at its base corners, a prism at its vertices), and the six shapes
    // added after this function was written are all inscribed in the same box --
    // so the corner distance is the correct bound for all of them.
    return (node.shape == Shape::Sphere) ? simd_reduce_max(half) : simd_length(half);
}

} // namespace sq
