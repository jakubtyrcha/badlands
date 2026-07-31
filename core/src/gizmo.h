#pragma once
#include <simd/simd.h>

#include <shapeshifter/ShapeshifterCore.h>   // GizmoHandle (interop enum)
#include "camera.h"                          // Camera

namespace sq {

struct Node;

// Gizmo apparent size: half_extent as a fraction of viewport height at the
// frame origin's depth (screen-constant, R1 user ruling — a world-constant
// gizmo would shrink to ~3% of the view at the camera's 90-unit radius
// clamp and swallow the screen fully zoomed in).
inline constexpr float kGizmoScreenFraction = 0.12f;

// The modify-mode move gizmo's tangent frame. origin/n follow
// drag_plane_for_node (snapped -> snap frame, unsnapped -> camera-facing);
// (u, v, n) is orthonormal and right-handed (u x v == n).
struct GizmoFrame {
    simd_float3 origin;
    simd_float3 n, u, v;
    float half_extent;
};

// u = normalize(cross(n, ref)), v = cross(n, u), ref = {0,1,0} unless n is
// nearly vertical (|n.y| >= 0.99), then {1,0,0} — the single source of the
// formula lines.h documents for the grid. Degenerate ||n|| ~ 0 falls back to
// n = {0,1,0} rather than emitting NaNs; upstream normals are unit by
// construction, so this is defense-in-depth only.
void tangent_basis(simd_float3 n, simd_float3& u, simd_float3& v);

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera);

} // namespace sq
