#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <optional>
#include "camera.h"     // sq::Ray

namespace sq {

class SceneDocument;
struct Node;

struct RayHit {
    float t;                 // parametric distance along the (normalized) query ray
    simd_float3 point;
    simd_float3 normal;      // unit length
};

// Sphere-traces ONE node's own SDF -- the same sdf_eval_node the renderer
// evaluates, so picking cannot disagree with what is on screen about where a
// surface is. Replaces the hand-written ray_unit_cube/ray_unit_sphere pair,
// which had no general form for the six shapes added after them and would have
// needed a bespoke intersector each.
//
// The returned hit is WORLD-SPACE, `t` in world units. That falls out of doing
// the transform rigidly: world -> local here is rotation and translation only
// (NOT world_from_local's inverse, which also divides out scale and lands in a
// warped space), and a rigid map preserves distance -- so the marched parameter
// is already a world distance and needs no recovery step.
//
// `world.dir` is normalized on entry, so `t` is a true distance regardless of
// what the caller passed.
//
// Known and deliberate: the hit lands within the trace's epsilon of the surface
// rather than exactly on it, and a grazing ray can exhaust the step budget and
// report a miss -- the same artifact the raymarched viewport already accepts at
// silhouettes. Nothing downstream needs exactness: a snapped spawn places the
// new node's CENTRE on this point.
std::optional<RayHit> raycast_node(const Node& node, const Ray& world);

struct PickHit { int32_t node_id; RayHit hit; };           // hit is world-space, t in world units

// Nearest node hit along the ray. CSG-blind by design, unchanged from the
// analytic version: each node is traced on its own, so a region carved away by
// a Subtract node still reports a hit, and a Subtract node is itself pickable.
std::optional<PickHit> raycast_scene(const SceneDocument& doc, const Ray& world);

// Intersect a ray with a plane. Returns the world-space intersection point,
// or nullopt when the ray is parallel to the plane (|dot(dir, n)| < 1e-6)
// or the intersection is at/behind the ray origin (t <= 1e-4).
std::optional<simd_float3> ray_plane(const Ray& ray, simd_float3 plane_point, simd_float3 plane_normal);

} // namespace sq
