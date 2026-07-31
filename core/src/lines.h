#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <vector>
#include <shared_types.h>

namespace sq {
class SceneDocument;

inline constexpr simd_float4 kColorAdd      = {0.1f, 1.0f, 0.1f, 1.0f};
inline constexpr simd_float4 kColorSubtract = {1.0f, 0.2f, 0.15f, 1.0f};
inline constexpr simd_float4 kColorSelected = {0.6f, 0.8f, 1.0f, 1.0f};
inline constexpr simd_float4 kColorGridLine = {1.0f, 1.0f, 1.0f, 0.18f};
inline constexpr simd_float4 kColorGridAxis = {1.0f, 1.0f, 1.0f, 0.9f};

inline constexpr int kSphereOutlineSegments = 48;

// 12 edges -> 24 vertices. Unit cube corners at ±0.5.
void append_cube_edges(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color);
// View-dependent silhouette outline of the local unit sphere (radius 0.5),
// after https://iquilezles.org/articles/sphereproj/: the horizon circle — the
// locus of points where the eye ray is tangent to the sphere — projects to
// exactly the sphere's screen-space outline ellipse. Computed in local space
// (eye transformed by inverse(world_from_local)), so non-uniformly scaled
// spheres (ellipsoids) are handled exactly; vertices come back through
// world_from_local. kSphereOutlineSegments segments -> 2*kSphereOutlineSegments
// vertices; emits nothing when the eye is inside (or on) the sphere.
void append_sphere_outline(std::vector<LineVertex>& out, const simd_float4x4& world_from_local,
                           simd_float4 color, simd_float3 eye_world);
// Color per node op (Add=kColorAdd, Subtract=kColorSubtract); selected node overrides to
// kColorSelected. eye_world feeds the sphere outlines, making the result view-dependent.
// Wireframe policy: mesh_present=false emits every node's wireframe (pre-D7 behavior, still
// used once no mesh has been reconstructed yet, e.g. an empty scene). mesh_present=true emits
// ONLY the selected node's wireframe (nothing when selected_id is invalid) -- once the shaded
// mesh is on screen, drawing every node's edges on top would be visual clutter; the wireframe
// becomes a selection annotation instead.
std::vector<LineVertex> build_scene_lines(const SceneDocument& doc, int32_t selected_id, simd_float3 eye_world,
                                          bool mesh_present);

// Floating tangent-frame grid + axes, centered at `origin` in the plane with unit `normal`.
// Tangent basis: u = normalize(cross(n, |n.y| < 0.99 ? {0,1,0} : {1,0,0})), v = cross(n, u).
// Emits, for even `divisions` (with he = half_extent, step = 2*he/divisions):
//  - grid lines i in 0..divisions along BOTH u and v directions, SKIPPING the center line
//    (i == divisions/2) in each direction: 2*divisions lines, color kColorGridLine
//  - 2 axis lines through the origin (u from -he to +he, v likewise), color kColorGridAxis
//  - 1 normal stub: origin -> origin + n * (0.5 * he), color kColorGridAxis
// Total for divisions=12: 24 + 2 + 1 = 27 lines = 54 vertices (54 * 32B = 1728B, safely
// under the 4KB setVertexBytes limit).
void append_tangent_frame(std::vector<LineVertex>& out, simd_float3 origin, simd_float3 normal,
                          float half_extent, int divisions);
}
