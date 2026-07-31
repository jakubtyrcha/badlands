#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace sq {

class SceneDocument;

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
