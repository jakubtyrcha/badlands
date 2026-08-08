#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <optional>
#include <vector>

#include "sdf_scene.h" // SdfNode, dual-compile CSG fold (shared with shaders/raymarch.metal)

namespace sq {

class SceneDocument;
struct Node;

// The node as sdf_eval_node sees it once its world transform is stripped off:
// centred at the origin with identity rotation, half-extents and shape
// parameter intact. Callers that have already moved a query point into the
// node's rigid frame -- picking's trace, the wireframe's profile sampling --
// evaluate against this, so there is exactly one place that mapping is written.
//
// half_extents comes in rather than being read off the node, because a node's
// WORLD box is its own scale times whatever uniform scale it inherits, and only
// the document can answer that (SceneDocument::placement().half_extents).
SdfNode local_sdf_node(const Node& node, simd_float3 half_extents);

// Regular sampling grid: n samples per axis over a cubic domain, sample
// (0,0,0) at `origin`, `spacing` between adjacent samples along each axis.
// GPU-ready layout: POD description + flat sample vector, x-fastest indexing
// (index = x + n*(y + n*z)) — no pointer graphs.
struct SampleGrid {
    simd_float3 origin = {0.0f, 0.0f, 0.0f};  // world position of sample (0,0,0)
    float spacing = 0.0f;                     // distance between adjacent samples
    int32_t n = 0;                            // samples per axis (n >= 2 for a non-empty scene); cells per axis = n - 1
    std::vector<float> values;                // size n*n*n, index = x + n*(y + n*z)
};

// Exact box SDF in the box's own frame: q relative to the box center,
// half_extents = per-axis half sizes. Exact under nonuniform half_extents.
float sd_box(simd_float3 q, simd_float3 half_extents);

// Ellipsoid SDF (iq's approximation) in the ellipsoid's own frame: q relative
// to its center, radii = per-axis radii. Reduces to the exact `length(q) - r`
// when radii are uniform. q == 0 is a singularity of the approximation
// (division by zero); guarded to return -min_component(radii), the exact
// distance to the nearest surface point from the center.
float sd_ellipsoid(simd_float3 q, simd_float3 radii);

// Packs the scene's nodes into SdfNode array, in document order (see
// sdf_scene.h for the exact SdfNode field packing). Every node is packed --
// no cap: the raymarch node upload is a real MTL::Buffer sized to the
// packed byte length each frame (see renderer.cpp), not a fixed-size
// setFragmentBytes binding, so there is no fixed budget to truncate against.
std::vector<SdfNode> pack_scene(const SceneDocument& doc);

// Out-param overload: same packing as above, but clears and fills the
// caller-owned `out` vector in place instead of returning a fresh one. Lets a
// per-frame caller (the raymarch renderer) reuse one scratch vector across
// frames -- `out`'s capacity is only grown on the first call or two (until it
// reaches the scene's steady-state node count), never reallocated every
// frame after that.
void pack_scene(const SceneDocument& doc, std::vector<SdfNode>& out);

// Evaluates the scene's CSG SDF at a world-space point by combining every
// node's local SDF in document order (Add -> min(d, d_node), Subtract ->
// max(d, -d_node), starting d = +FLT_MAX). Nullopt for an empty scene (no
// nodes to evaluate).
std::optional<float> evaluate_scene_sdf(const SceneDocument& doc, simd_float3 p);

// Samples the scene SDF onto a regular n x n x n grid over a cube inflated
// from the scene's AABB (see sdf.cpp for the exact domain formula). Empty
// scene -> empty SampleGrid (default-constructed; n is ignored). n must be
// >= 2 for a non-empty scene.
SampleGrid sample_scene(const SceneDocument& doc, int32_t n);

} // namespace sq
