#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <optional>
#include "camera.h"     // sq::Ray
#include "frame.h"      // sq::NodePlacement

namespace sq {

class SceneDocument;
struct Node;

struct RayHit {
    float t;                 // parametric distance along the (normalized) query ray
    simd_float3 point;
    simd_float3 normal;      // unit length
};

// Sphere-traces ONE node's own SDF -- the same sdf_eval_node the renderer
// evaluates, since sdf_scene.h compiles as C++, MSL and Slang from one source.
// Replaces the hand-written ray_unit_cube/ray_unit_sphere pair, which had no
// general form for the six shapes added after them and would have needed a
// bespoke intersector each.
//
// THAT AGREEMENT IS FREE, NOT REQUIRED, and the difference matters. It used to
// be stated here as the guarantee picking rests on. It no longer is: rendering
// may move to a splat representation sharing nothing with this evaluator, and
// picking has to survive that. What picking actually rests on is finding the
// PRIMITIVE where the primitive is -- pinned per shape, to a stated tolerance,
// in tests/core/picking_tests.cpp. Nothing here needs to match the rendered
// image pixel for pixel: a click selects a node, and a snapped spawn places the
// new node's centre on the hit.
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
//
// `placement` supplies WHERE the node is; `node` supplies only what it is
// (shape and profile parameter). The pair is required rather than the node
// alone because a Node no longer determines its own world placement -- that is
// the document's answer to give (SceneDocument::placement). Callers holding a
// document use raycast_scene below; this overload exists for the pieces it is
// built from and for tests that place one node explicitly.
std::optional<RayHit> raycast_node(const Node& node, const NodePlacement& placement,
                                   const Ray& world);

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
