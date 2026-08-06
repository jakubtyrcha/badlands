#include "picking.h"

#include <cmath>

#include "scene.h"
#include "sdf_scene.h"  // SdfNode, sdf_eval_node -- the same evaluator the renderer traces

namespace sq {

namespace {

constexpr float kEps = 1e-4f;

// Sphere-trace tunables. Tighter than the viewport's (raymarch.metal uses a
// distance-scaled 5e-4) because this runs a handful of times per mouse move
// rather than once per pixel, and because the hit point becomes a spawn's
// position -- so the budget is better spent here than there.
constexpr float kTraceHitEps = 1e-5f;
constexpr float kTraceMaxDist = 1e3f;   // far beyond the camera's 90-unit radius clamp
constexpr int kTraceMaxSteps = 192;
// Floor on a step, so a ray running exactly along a surface cannot stall and
// burn the whole budget without advancing.
constexpr float kTraceMinStep = 1e-6f;
// Central-difference offset for the normal. Comfortably above kTraceHitEps so
// the four taps straddle the surface rather than all landing inside its
// tolerance band, which would leave the gradient dominated by float noise.
constexpr float kNormalEps = 1e-3f;

// The node as the evaluator sees it, with the world transform stripped out:
// centred at the origin with identity rotation, because raycast_node has
// already moved the ray into that frame. Reusing sdf_eval_node rather than
// re-deriving a local evaluator is the point -- it is what makes picking and
// rendering answer with the same surface by construction.
SdfNode local_node(const Node& node) {
    const simd_float3 half = node.scale * 0.5f;
    SdfNode sn;
    sn.pos_shape = sdf_make4(0.0f, 0.0f, 0.0f,
                             static_cast<float>(static_cast<int32_t>(node.shape)));
    sn.half_extents_op = sdf_make4(half.x, half.y, half.z, 0.0f); // op is not read here
    sn.inv_rotation = sdf_make4(0.0f, 0.0f, 0.0f, 1.0f);          // identity
    sn.params = sdf_make4(node.shape_param, 0.0f, 0.0f, 0.0f);
    return sn;
}

// Tetrahedron-offset gradient (iq's): 4 evaluations rather than the 6 a naive
// central difference needs, and the same pattern raymarch.metal shades with.
// Points outward on an exit hit as well as an entry one, which is the
// convention the analytic face normals used to produce by hand.
simd_float3 local_normal(const SdfNode& sn, simd_float3 q) {
    const simd_float3 e1 = {1.0f, -1.0f, -1.0f};
    const simd_float3 e2 = {-1.0f, -1.0f, 1.0f};
    const simd_float3 e3 = {-1.0f, 1.0f, -1.0f};
    const simd_float3 e4 = {1.0f, 1.0f, 1.0f};
    return simd_normalize(e1 * sdf_eval_node(sn, q + e1 * kNormalEps) +
                          e2 * sdf_eval_node(sn, q + e2 * kNormalEps) +
                          e3 * sdf_eval_node(sn, q + e3 * kNormalEps) +
                          e4 * sdf_eval_node(sn, q + e4 * kNormalEps));
}

} // namespace

std::optional<RayHit> raycast_node(const Node& node, const Ray& world) {
    const float dir_len = simd_length(world.dir);
    if (!(dir_len > 0.0f)) {
        return std::nullopt;
    }
    const simd_float3 dir = world.dir / dir_len;

    // Rigid world -> local. No simd_inverse and no scale division: scale stays
    // baked into the half-extents the evaluator measures against, exactly as it
    // does for rendering, so this frame differs from world by a rotation and a
    // translation only -- which is what lets `t` mean a world distance.
    const simd_quatf inv_rotation = simd_conjugate(node.rotation);
    const simd_float3 o = simd_act(inv_rotation, world.origin - node.position);
    const simd_float3 d = simd_act(inv_rotation, dir);

    const SdfNode sn = local_node(node);

    // Start at kEps rather than 0, which is what makes a hit at or behind the
    // origin impossible by construction -- the guard the analytic version
    // spelled out as an explicit `world_t <= kEps` rejection afterwards.
    float t = kEps;
    for (int step = 0; step < kTraceMaxSteps && t < kTraceMaxDist; ++step) {
        const simd_float3 q = o + t * d;
        const float dist = sdf_eval_node(sn, q);
        // ABSOLUTE value, which is the whole of what preserves the behaviour
        // that an origin INSIDE a node returns that node's exit face -- and the
        // eye really does end up inside geometry, because dollying in is
        // unclamped. From inside, |d| is still a lower bound on the distance to
        // the boundary, so stepping by it cannot overshoot the exit surface.
        const float advance = std::fabs(dist);
        if (advance < kTraceHitEps) {
            const simd_float3 normal = local_normal(sn, q);
            return RayHit{t, node.position + simd_act(node.rotation, q),
                          simd_act(node.rotation, normal)};
        }
        t += std::fmax(advance, kTraceMinStep);
    }
    return std::nullopt;
}

std::optional<PickHit> raycast_scene(const SceneDocument& doc, const Ray& world) {
    std::optional<PickHit> best;
    for (const Node& node : doc.nodes()) {
        const std::optional<RayHit> hit = raycast_node(node, world);
        if (hit && (!best || hit->t < best->hit.t)) {
            best = PickHit{node.id, *hit};
        }
    }
    return best;
}

std::optional<simd_float3> ray_plane(const Ray& ray, simd_float3 plane_point, simd_float3 plane_normal) {
    const float denom = simd_dot(ray.dir, plane_normal);
    if (std::fabs(denom) < 1e-6f) {
        return std::nullopt; // ray parallel to the plane
    }

    const float t = simd_dot(plane_point - ray.origin, plane_normal) / denom;
    if (t <= kEps) {
        return std::nullopt; // intersection at/behind the ray origin
    }

    return ray.origin + t * ray.dir;
}

} // namespace sq
