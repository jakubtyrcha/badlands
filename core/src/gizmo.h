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

// A gizmo's frame. (u, v, n) is orthonormal and right-handed (u x v == n) for
// both kinds; what differs is where it comes from.
//
// Move: origin/n follow drag_plane_for_node (snapped -> snap frame, unsnapped
// -> camera-facing), because a move handle drags along a surface.
//
// Scale: the node's own local axes -- world X/Y/Z while Node::rotation stays
// identity (scene.h) -- centred on the node. A camera-facing basis would be
// meaningless here: a scale handle has to map onto a scale COMPONENT, and
// u/v/n then land on scale.x/y/z directly. It also makes the colours correct
// for free, since kColorAxisU/V/N already alias the world-axis palette.
struct GizmoFrame {
    simd_float3 origin;
    simd_float3 n, u, v;
    float half_extent;
};

// --- scale gizmo -----------------------------------------------------------
//
// Three axis shafts plus a centre box for uniform scale. No plane patches and
// no grid: the grid is a drag-PLANE affordance, and scale has no drag plane.

// The centre (uniform) handle's drawn half-size, and where the axis shafts
// start, both as fractions of half_extent. Together they PARTITION the gizmo:
// the centre owns the disc around the origin, the axes own everything outboard
// of it, so "aim at the middle" can never be answered by an axis.
inline constexpr float kGizmoUniformHalfSizeFrac = 0.09f;
inline constexpr float kScaleAxisInnerFrac = 0.18f;
// Box tips, larger than the move gizmo's terminator dots: box-tipped axes are
// the universal scale convention, and the size difference is what tells the
// two gizmos apart at a glance without new 3D geometry.
inline constexpr float kGizmoScaleTipHalfSizeFrac = 0.055f;

// Scale's axis grab tolerance, deliberately wider than Move's
// kAxisPickTolerancePts. With no plane patches and no grid, the scale gizmo's
// interactable footprint is far smaller than Move's -- and under the always-on
// camera a near-miss now spins the view rather than doing nothing, so these
// handles have to be MORE forgiving, not less. The hover highlight
// (updateGizmoHover) is the pre-press signal that a handle is live, which is
// what keeps this from needing an invisible dead zone around the gizmo.
inline constexpr float kScaleAxisPickTolerancePts = 14.0f;
inline constexpr float kUniformPickTolerancePts = 14.0f;

// Axis-scale drag reads the closest-approach parameter as a ratio:
// factor = s_now / s_start. Flooring BOTH the captured start and every update
// at this fraction of he keeps that factor positive, finite, and exactly 1 at
// mouse-down -- so there is no divide-by-zero at the origin, no sign flip when
// dragged through it, and no jump on grab. Rejecting the grab instead would
// hand the gesture to the camera, which is the one outcome a user aiming at a
// handle never wants.
inline constexpr float kScaleAxisMinGrabFrac = 0.15f;

// Floored closest-approach parameter for an axis handle, per the above.
// nullopt only when ray_axis_param itself is singular (near-parallel).
std::optional<float> scale_axis_param(const Ray& ray, const GizmoFrame& frame, GizmoHandle handle);

// Component index (0=x, 1=y, 2=z) an axis handle scales, following the frame's
// u/v/n -> local x/y/z mapping documented on GizmoFrame. Non-axis handles
// return -1.
int gizmo_scale_axis_index(GizmoHandle handle);

// u = normalize(cross(n, ref)), v = cross(n, u), ref = {0,1,0} unless n is
// nearly vertical (|n.y| >= 0.99), then {1,0,0} — the single source of the
// formula lines.h documents for the grid. Degenerate ||n|| ~ 0 falls back to
// n = {0,1,0} rather than emitting NaNs; upstream normals are unit by
// construction, so this is defense-in-depth only.
void tangent_basis(simd_float3 n, simd_float3& u, simd_float3& v);

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera, GizmoKind kind);

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
// plane patches origin + x*e1 + y*e2 with x, y in
// [kGizmoPatchInner*he, kGizmoPatchOuter*he] (ray_plane + bounds). Named
// rather than spelled out on purpose -- this comment previously restated the
// bounds as literals and went stale the moment they moved, which is the exact
// failure those constants exist to prevent. Any axis hit beats any plane hit;
// among axes smallest distance wins (ties: smaller ray-t, then declaration
// order); among planes nearest ray-t wins.
// `kind` selects the handle set, and is REQUIRED rather than defaulted: which
// manipulator is being hit-tested is exactly the kind of thing that must not
// be decided silently at a call site.
//
// Move: axes over 0..he, then plane patches. Scale: the uniform centre first
// (it owns the disc the axes are pulled back from), then axes over
// kScaleAxisInnerFrac*he..he at the wider scale tolerance, and no planes.
GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts, GizmoKind kind);

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
