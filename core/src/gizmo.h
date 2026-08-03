#pragma once
#include <simd/simd.h>
#include <optional>

#include <shapeshifter/ShapeshifterCore.h>   // GizmoHandle (interop enum)
#include "camera.h"                          // Camera, Ray

namespace sq {

struct Node;

// Gizmo apparent size: half_extent as a fraction of viewport height at the
// frame origin's depth (screen-constant, R1 user ruling — a world-constant
// gizmo would shrink to ~3% of the view at the camera's 90-unit radius
// clamp and swallow the screen fully zoomed in). 0.24 = the R2 "2x larger"
// user ruling over the original 0.12.
inline constexpr float kGizmoScreenFraction = 0.24f;

// Plane-handle patch bounds, as fractions of half_extent: the patch is the
// square origin + x*e1 + y*e2 with x, y in [inner*he, outer*he].
//
// SINGLE SOURCE OF TRUTH. "Drawn = hit" is load-bearing for this gizmo, and
// before these existed the same two numbers were spelled out independently in
// lines.cpp (the draw), gizmo.cpp (the pick) and three test files -- so a
// restyle could silently move the drawn patch off its own hit region. Any
// change here moves both halves together, by construction.
inline constexpr float kGizmoPatchInner = 0.24f;
inline constexpr float kGizmoPatchOuter = 0.50f;
// The point a caller should aim at to hit a patch dead centre (tests, and any
// future "grab the plane handle" affordance).
inline constexpr float kGizmoPatchCenter = 0.5f * (kGizmoPatchInner + kGizmoPatchOuter);

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

// Screen-constant axis grab tolerance, converted to world units at the
// candidate point's depth inside pick_gizmo_handle (same scale family as
// CameraController::pan_view).
inline constexpr float kAxisPickTolerancePts = 8.0f;
// An axis seen nearly end-on (|dot(ray, axis)| above this) is skipped
// entirely: the drag solver's parameter explodes long before its 1e-6
// denominator cutoff (sin^2 of the angle), so an axis that can't be dragged
// stably must not be grabbable or hoverable either.
inline constexpr float kAxisViewAlignLimit = 0.995f;

// Closest-approach parameter s of `ray` to the line origin + s*axis_dir
// (axis_dir unit). nullopt when near-parallel (denominator < 1e-6) — drag
// callers keep the last position, mirroring ray_plane's parallel guard.
std::optional<float> ray_axis_param(const Ray& ray, simd_float3 origin, simd_float3 axis_dir);

// Drawn geometry = hit geometry: axes 0..+he along u/v/n (positive half
// only, R3 ruling; ray-to-segment distance vs the pts tolerance above),
// plane patches origin + x*e1 + y*e2, x,y in [0.3he, 0.6he] (ray_plane +
// bounds). Any axis hit beats any plane hit; among axes smallest distance
// wins (ties: smaller ray-t, then declaration order); among planes nearest
// ray-t wins.
GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts);

// Camera-pivot marker visibility over time. Full strength while a gesture is
// running and for kPivotHoldSeconds after the last camera move, then a
// smoothstep to nothing over kPivotFadeSeconds.
//
// Time-based rather than frame-based on purpose: the caller feeds elapsed
// seconds from a steady_clock, so the fade takes the same wall-clock duration
// regardless of refresh rate. A per-frame decrement would quantize whenever
// the display drops to a lower rate.
//
// Pure so it is directly testable; Editor supplies the elapsed time. Inputs
// at or below zero return 1.0.
inline constexpr float kPivotHoldSeconds = 0.25f;
inline constexpr float kPivotFadeSeconds = 0.35f;
float pivot_marker_alpha(float seconds_since_activity);

// Handle -> frame-vector mapping shared by the drag solver and rendering.
// axis_dir: the pull direction of an axis handle. plane_normal: the drag
// plane of a plane handle is spanned by its two basis vectors, so its normal
// is the pair's THIRD basis vector. Both expect their own handle kind.
bool gizmo_handle_is_axis(GizmoHandle handle);
simd_float3 gizmo_axis_dir(const GizmoFrame& frame, GizmoHandle handle);
simd_float3 gizmo_plane_normal(const GizmoFrame& frame, GizmoHandle handle);

} // namespace sq
