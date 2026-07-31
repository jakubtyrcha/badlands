#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <vector>
#include <shared_types.h>

#include "gizmo.h"   // GizmoFrame/GizmoHandle for append_move_gizmo

namespace sq {
class SceneDocument;

inline constexpr simd_float4 kColorAdd      = {0.1f, 1.0f, 0.1f, 1.0f};
inline constexpr simd_float4 kColorSubtract = {1.0f, 0.2f, 0.15f, 1.0f};
inline constexpr simd_float4 kColorSelected = {0.6f, 0.8f, 1.0f, 1.0f};
inline constexpr simd_float4 kColorGridLine = {1.0f, 1.0f, 1.0f, 0.18f};

// Gizmo handle colors (user ruling): RGB per axis, additive mix per plane
// (uv = red+green = yellow, etc). Hot is WHITE — amber would collide with
// the yellow uv plane.
inline constexpr simd_float4 kColorAxisU    = {1.0f, 0.0f, 0.0f, 1.0f};
inline constexpr simd_float4 kColorAxisV    = {0.0f, 1.0f, 0.0f, 1.0f};
inline constexpr simd_float4 kColorAxisN    = {0.0f, 0.0f, 1.0f, 1.0f};
inline constexpr simd_float4 kColorPlaneUV  = {1.0f, 1.0f, 0.0f, 0.9f};
inline constexpr simd_float4 kColorPlaneUN  = {1.0f, 0.0f, 1.0f, 0.9f};
inline constexpr simd_float4 kColorPlaneVN  = {0.0f, 1.0f, 1.0f, 0.9f};
inline constexpr simd_float4 kColorGizmoHot = {1.0f, 1.0f, 1.0f, 1.0f};

// Handle line half-width as a fraction of the frame's half_extent. he is
// screen-constant, so this is screen-constant thickness too (~4.8pts full
// width at the default kGizmoScreenFraction on a 500pt-tall viewport).
inline constexpr float kGizmoHandleHalfWidthFrac = 0.02f;

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
// Wireframe policy (subtract-always + selected-always-overrides): per node --
// the selected node (either op) always draws in kColorSelected, overriding
// everything else; otherwise an unselected Subtract node still draws, in
// kColorSubtract; otherwise (an unselected Add node) nothing is emitted. The
// raymarched view (R1) always shows the full scene live, so additive geometry
// is already visible there -- but a Subtract node's carve is its ONLY visual
// anywhere in the viewport, so it keeps a wireframe even unselected (without
// this exception it would be entirely unpickable-by-sight). eye_world feeds
// the sphere outline, making the result view-dependent.
std::vector<LineVertex> build_scene_lines(const SceneDocument& doc, int32_t selected_id, simd_float3 eye_world);

// The modify-mode move gizmo, drawn from the same GizmoFrame the hit-testing
// uses (gizmo.h) so drawn geometry and pick geometry cannot drift. Split into
// two appends because the two halves draw with different primitives:
//
// Grid (thin LINE primitives, decoration): grid lines i in 0..divisions
// along BOTH u and v directions, SKIPPING the center line (i == divisions/2)
// in each direction: 2*divisions lines = 48 verts at divisions=12, color
// kColorGridLine.
void append_move_gizmo_grid(std::vector<LineVertex>& out, const GizmoFrame& frame, int divisions);
//
// Handles (TRIANGLE primitives): each handle line is expanded into a
// camera-facing quad of half-width kGizmoHandleHalfWidthFrac * he (Metal has
// no line width), endpoints extended by the half-width so segments sharing a
// corner overlap instead of notching. Emits, in this order (pinned by
// lines_tests):
//  - 3 axis segments through the origin, -he..+he along u/v/n (n is a full
//    axis — the 1-DOF off-plane pull), colors kColorAxisU/V/N
//  - 3 plane-patch outlines (uv, un, vn), the [0.3he, 0.6he]^2 square in
//    each basis pair (4 segments each), colors kColorPlaneUV/UN/VN
// The `highlighted` handle draws in kColorGizmoHot instead of its base
// color (hover/active feedback). A segment pointing straight at the eye has
// no on-screen extent and is skipped. Total: 15 segments * 6 = 90 verts
// (2880B, under the 4KB setVertexBytes limit; the grid's 48 verts ride in a
// separate draw).
void append_move_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                               GizmoHandle highlighted, simd_float3 eye);
}
