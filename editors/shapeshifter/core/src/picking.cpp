#include "picking.h"

#include <cmath>

#include "scene.h"
#include "sdf.h"        // local_sdf_node -- the shared world-transform-stripped node
#include "sdf_scene.h"  // SdfNode, sdf_eval_node -- the same evaluator the renderer traces

namespace sq {

namespace {

constexpr float kEps = 1e-4f;

// Sphere-trace tunables. Tighter than the viewport's (raymarch.metal uses a
// distance-scaled 5e-4) because this runs a handful of times per mouse move
// rather than once per pixel, and because the hit point becomes a spawn's
// position -- so the budget is better spent here than there.
// DISTANCE-SCALED, like the viewport's, and this is a fix rather than a tuning.
// An ABSOLUTE epsilon was the bug: the shape SDFs are bounds, not exact
// distances, and the cross-section contraction makes them underestimate badly
// on a flattened box -- so the trace crawls toward the surface in ever-smaller
// steps and exhausts its budget on rays that pass straight through the solid.
// Measured on a sphere at aspect 20, an absolute 1e-5 missed 21.8% of interior
// rays, and raising the budget to 1024 steps STILL missed 0.3% at 982 steps.
// The budget was never the problem.
//
// Kept 10x tighter than the viewport's 5e-4*t, because the reason picking chose
// tighter still holds: this runs a handful of times per mouse move rather than
// once per pixel, and the hit point becomes a spawn's position.
inline float trace_hit_eps(float t) { return std::fmax(1e-5f, 5e-5f * t); }
constexpr float kTraceMaxDist = 1e3f;   // far beyond the camera's 90-unit radius clamp
// 384, not 192: at aspect 20 the scaled epsilon needs 261 steps in the worst
// case. An isotropic shape still hits in ONE step, so this costs the common
// case nothing.
constexpr int kTraceMaxSteps = 384;
// Floor on a step, so a ray running exactly along a surface cannot stall and
// burn the whole budget without advancing.
constexpr float kTraceMinStep = 1e-6f;
// Central-difference offset for the normal. ABSOLUTE while the hit epsilon
// above is distance-scaled, so past t = 20 the accepted hit sits further from
// the surface than the taps that measure the gradient there -- and the camera's
// 90-unit radius clamp makes that the ordinary case, not an exotic one.
//
// It survives that anyway, which is why this is a fixed constant and not a
// function of t: the SDF is smooth, so the tap-to-tap difference stays near
// 1.15e-3 while float error at t = 90 is a few parts in 1e6. Measured normals
// agree with the analytic sphere to within a dot product of 1.0 at 2, 20, 60
// and 90 units out ("the surface normal holds up at picking distance",
// picking_tests.cpp) -- change either epsilon and that test is the arbiter.
constexpr float kNormalEps = 1e-3f;

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

std::optional<RayHit> raycast_node(const Node& node, const NodePlacement& placement,
                                   const Ray& world) {
    const float dir_len = simd_length(world.dir);
    if (!(dir_len > 0.0f)) {
        return std::nullopt;
    }
    const simd_float3 dir = world.dir / dir_len;

    // Rigid world -> local. No simd_inverse and no scale division: scale stays
    // baked into the half-extents the evaluator measures against, exactly as it
    // does for rendering, so this frame differs from world by a rotation and a
    // translation only -- which is what lets `t` mean a world distance.
    //
    // That the resolved frame is rigid is guaranteed by Frame's own shape
    // (position + rotation + one uniform scalar, no matrix), not merely by
    // convention -- so no amount of parenting can make this map non-rigid and
    // quietly turn `t` into something other than a distance.
    const simd_quatf inv_rotation = simd_conjugate(placement.frame.rotation);
    const simd_float3 o = simd_act(inv_rotation, world.origin - placement.frame.position);
    const simd_float3 d = simd_act(inv_rotation, dir);

    // Reusing sdf_eval_node rather than re-deriving a local evaluator is the
    // point: it is what makes picking and rendering answer with the same
    // surface by construction.
    const SdfNode sn = local_sdf_node(node, placement.half_extents);

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
        if (advance < trace_hit_eps(t)) {
            const simd_float3 normal = local_normal(sn, q);
            return RayHit{t, placement.frame.position + simd_act(placement.frame.rotation, q),
                          simd_act(placement.frame.rotation, normal)};
        }
        t += std::fmax(advance, kTraceMinStep);
    }
    return std::nullopt;
}

std::optional<PickHit> raycast_scene(const SceneDocument& doc, const Ray& world) {
    std::optional<PickHit> best;
    for (const Node& node : doc.nodes()) {
        const std::optional<RayHit> hit = raycast_node(node, doc.placement(node.id), world);
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
