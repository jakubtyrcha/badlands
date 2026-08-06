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
// both kinds; what differs is where it comes from. NOTHING here depends on the
// camera except half_extent -- a manipulator whose axes swim as you orbit
// cannot build muscle memory, which is the whole point of this frame rework.
//
// Move, attached: {snap_point, snap_normal} plus its tangent basis, because a
// move handle on a detail drags along the surface the detail sits on.
//
// Move, free / Scale: the node's own local axes (simd_act of Node::rotation on
// world X/Y/Z), centred on the node. For Scale this is forced -- a scale handle
// has to map onto a scale COMPONENT, so u/v/n must land on scale.x/y/z. For a
// free Move it is a choice, reversing the original move-gizmo ruling that let
// an unsnapped node's axes follow the camera. Either way the colours come out
// right for free, since kColorAxisU/V/N already alias the world-axis palette
// and these reduce to world X/Y/Z at identity rotation.
//
// grid_normal is the normal of the plane the GRID is drawn in, and is
// deliberately independent of n. For an attached node the two agree: the
// tangent plane is both the surface you slide on and the u-v drag plane. For a
// free node the grid is world-horizontal (grid_normal = +Y) -- a local echo of
// the ground plate, which is more use than the vertical wall the u-v plane
// would give. So the grid is a REFERENCE plane, not a drag-plane affordance as
// it was originally specified. Scale draws no grid and leaves this at n.
struct GizmoFrame {
    simd_float3 origin;
    simd_float3 n, u, v;
    simd_float3 grid_normal;
    float half_extent;
};

// --- radius bands ----------------------------------------------------------
//
// Both gizmos are live at once, and when their anchors coincide their handles
// share an origin. Each handle is laid out in its own band of radius, as a
// fraction of half_extent, so the combined gizmo reads as the union of the two
// separate ones and neither restructures itself as the pair merges or splits.
//
//   0.00 .. 0.09   Shape: uniform scale (the centre disc)
//   0.00 .. 0.60   Placement: move axes
//   0.24 .. 0.50   Placement: move plane patches (off-axis, so never on a shaft)
//   0.70 .. 1.00   Shape: scale axes
//   1.15           Placement: rotation rings (a radius, not a range)
//
// These are the DRAWN extents, and they are not the whole story for picking:
// every handle also carries a screen-space grab tolerance (8-14 pts), which the
// fractions below know nothing about. Since he is kGizmoScreenFraction of the
// viewport height, the 0.10 he gap between the move and scale bands is only
// ~0.024 * viewport_h points -- around 12 pts on a 500 pt viewport, i.e. less
// than the two tolerances that meet there. So the bands DO overlap in practice
// and the "dead zone" the gap suggests does not exist.
//
// That is fine, but only because pick_gizmos resolves the overlap explicitly
// rather than relying on the geometry to keep the handles apart: move axes beat
// scale axes, and the centre beats a move axis only when it is genuinely
// nearer. Widening the gaps enough to separate the tolerances would make the
// gizmo far larger than the model it sits on, which is a worse trade.
// The one thing the ordering does NOT have to arbitrate is the uniform centre
// against the scale axes, which the drawn radii really do keep apart.
//
// The same arithmetic applies to the 0.15 he between the scale axes and the
// rings: below roughly a 390 pt viewport the scale tolerance reaches the ring,
// and since scale axes are resolved first a ring grab near one of the three
// axis directions goes to scale instead. Accepted rather than fixed -- the fix
// is a bigger gizmo, and it only bites in a window small enough that the whole
// manipulator already fills half the viewport.
inline constexpr float kMoveAxisOuterFrac  = 0.60f;  // pick clamp
inline constexpr float kMoveAxisShaftFrac  = 0.55f;  // drawn shaft end, and where its tip dot sits
inline constexpr float kScaleAxisInnerFrac = 0.70f;
inline constexpr float kScaleAxisShaftFrac = 0.94f;  // drawn shaft end, and where its box tip sits
inline constexpr float kScaleAxisOuterFrac = 1.00f;  // pick clamp
inline constexpr float kRotateRingFrac     = 1.15f;  // ring radius, drawn and picked
// Ordering asserts, on the DRAWN extents only -- the grab tolerances are in
// points and cannot be compared against fractions of he here (see the note
// above). They pin the layout's intent, not a guarantee of non-overlap.
static_assert(kMoveAxisOuterFrac < kScaleAxisInnerFrac,
              "move axes must be drawn inboard of scale axes: they share three lines");
static_assert(kScaleAxisOuterFrac < kRotateRingFrac,
              "the rings must be drawn outboard of every axis");
static_assert(kMoveAxisShaftFrac <= kMoveAxisOuterFrac &&
                  kScaleAxisShaftFrac <= kScaleAxisOuterFrac,
              "a drawn shaft must stay inside its own pickable band (drawn = hit)");

// How close the two anchors have to be before the pair reads as one gizmo.
// Compared in WORLD units against half_extent, which is itself screen-constant
// -- so this is a screen-space test with none of a projection's behind-camera
// or w-near-zero edge cases. Its only consumer is the tether: coalescing needs
// no other special case, because two gizmos at the same origin already draw
// and hit-test as their union.
inline constexpr float kGizmoCoalesceFrac = 0.35f;
bool gizmos_coalesce(const GizmoFrame& placement, const GizmoFrame& shape);

// --- shape (scale) gizmo ---------------------------------------------------
//
// Three axis shafts plus a centre box for uniform scale. No plane patches and
// no grid: the grid is a reference plane the Placement gizmo owns, and scale
// has no drag plane of its own.

// The centre (uniform) handle's drawn half-size. With the axis shafts pulled
// out to kScaleAxisInnerFrac, the centre owns the disc around the origin
// outright, so "aim at the middle" can never be answered by an axis.
inline constexpr float kGizmoUniformHalfSizeFrac = 0.09f;
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
//
// It has to sit WELL below kScaleAxisInnerFrac, not just below it. The most a
// drag can ever shrink an axis is floor/s_start, and s_start is bounded below
// by the innermost grabbable point -- so a floor close to the inner bound caps
// the achievable shrink near 1 and the gesture feels dead exactly where the
// handle is hardest to grab. At 0.02 against the inner bound the worst case is
// tens of times, and the per-component clamp takes over long before that.
inline constexpr float kScaleAxisMinGrabFrac = 0.02f;
static_assert(kScaleAxisMinGrabFrac * 8.0f < kScaleAxisInnerFrac,
              "the axis floor must stay far below the innermost grabbable point, or a "
              "grab near the inner end of the shaft can barely shrink the node");

// The uniform handle's drag rate (per view point of vertical travel) and the
// per-component bounds every scale drag clamps to. The rate is unchanged from
// the pre-gizmo scale gesture: the centre handle IS that gesture, now anchored
// to a real affordance instead of swallowing every drag in the viewport.
inline constexpr float kUniformScaleSens = 0.005f;
inline constexpr float kNodeScaleMin = 0.05f;
inline constexpr float kNodeScaleMax = 50.0f;

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

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera, GizmoSlot slot);

// Screen-constant axis grab tolerance, converted to world units at the
// candidate point's depth inside pick_gizmo_handle (same points-to-world
// scale family as CameraController::pan_world).
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

// Hit-tests ONE gizmo's handle set. Drawn geometry = hit geometry: axes over
// their slot's band along u/v/n (positive half only, R3 ruling; ray-to-segment
// distance vs the pts tolerance above), plane patches origin + x*e1 + y*e2 with
// x, y in [kGizmoPatchInner*he, kGizmoPatchOuter*he] (ray_plane + bounds). All
// named rather than spelled out on purpose -- this comment once restated the
// bounds as literals and went stale the moment they moved, which is the exact
// failure those constants exist to prevent. Any axis hit beats any plane hit;
// among axes smallest distance wins (ties: smaller ray-t, then declaration
// order); among planes nearest ray-t wins.
//
// `slot` selects the handle set, and is REQUIRED rather than defaulted: which
// manipulator is being hit-tested is exactly the kind of thing that must not be
// decided silently at a call site.
//
// Placement: axes over 0..kMoveAxisOuterFrac*he, then plane patches. Shape: the
// uniform centre first (it owns the disc the axes are pulled back from), then
// axes over kScaleAxisInnerFrac..kScaleAxisOuterFrac at the wider scale
// tolerance, and no planes.
GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts, GizmoSlot slot);

// Hit-tests BOTH gizmos and resolves the winner. This is what input should
// call; the single-slot version above exists for the pieces it is built from
// and for tests.
//
// Resolution runs innermost band outwards -- Shape's uniform centre, then
// Placement's move axes, then Shape's scale axes, then Placement's rings and
// planes -- with ONE distance comparison at the front: the uniform centre only
// beats a move axis if it is genuinely closer to the ray.
//
// That comparison is not decoration. The bands are disjoint in radius from
// their own origin, but the two gizmos stop sharing an origin the moment the
// pair splits, and a screen-space contest cannot be settled by a rule about
// world-space radii. Sight down the tether -- lift a detail off its surface,
// then orbit to face the lift -- and the two origins project onto each other
// while Placement's axes stay fully visible around them. Fixed priority there
// swallowed handles that were drawn, lit on hover, and under the cursor.
GizmoHit pick_gizmos(const GizmoFrame& placement, const GizmoFrame& shape, const Ray& ray,
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

// --- rotation rings --------------------------------------------------------
//
// One ring per frame axis, at kRotateRingFrac * he, in the plane perpendicular
// to that axis. Placement only.

// Grab tolerance for a ring, in view points. Between Move's 8 and Scale's 14:
// a ring is a long target (you can grab it anywhere around its circumference)
// but a thin one, and it is the outermost thing on the gizmo, so a generous
// tolerance here starts eating into empty space that should drive the camera.
inline constexpr float kRotateRingPickTolerancePts = 10.0f;

// A ring seen nearly EDGE-ON is unusable and so is made ungrabbable, the same
// bargain kAxisViewAlignLimit strikes for an end-on axis -- but the geometry is
// inverted, hence a minimum rather than a maximum. Edge-on means the ray lies
// in the ring's plane, i.e. |dot(ray_dir, axis)| near zero, where ray_plane's
// intersection runs away to infinity and the angle the drag reads from it is
// dominated by cursor noise. ray_plane's own 1e-6 guard is far too permissive
// to catch that.
inline constexpr float kRotateRingViewAlignMin = 0.08f; // ~4.6 degrees off edge-on

// The axis a ring turns the node about: RingU -> u, RingV -> v, RingN -> n.
// The ring itself is drawn and picked in the plane perpendicular to it.
bool gizmo_handle_is_ring(GizmoHandle handle);
simd_float3 gizmo_ring_axis(const GizmoFrame& frame, GizmoHandle handle);

// Where a ray meets a ring's circle, as a unit direction from the frame origin
// in the ring's plane -- the quantity the rotation drag reads its angle from,
// captured at the press and recomputed on every update.
//
// nullopt when the ring is too close to edge-on (kRotateRingViewAlignMin), when
// ray_plane finds nothing, or when the intersection lands on the origin itself
// (where the direction is undefined). Callers keep the last rotation, mirroring
// how the move and scale drags treat their own singular cases.
std::optional<simd_float3> ring_drag_dir(const Ray& ray, const GizmoFrame& frame,
                                          GizmoHandle handle);

// Signed angle from `from` to `to` about `axis`, in (-pi, pi]. Both are unit
// vectors in the plane perpendicular to `axis` (ring_drag_dir's output), so
// cross(from, to) is parallel to axis and its signed length gives the sine.
//
// Taken about a KNOWN axis rather than as a shortest-arc quaternion between the
// two vectors: the general [dot, cross] form degenerates at exactly
// antiparallel, where the cross product vanishes and the axis of rotation
// becomes undefined. Here the axis is known a priori, so this stays
// well-conditioned right through +-pi and the rotation can never drift off the
// ring the user grabbed.
float signed_angle_about(simd_float3 from, simd_float3 to, simd_float3 axis);

// Handle -> frame-vector mapping shared by the drag solver and rendering.
// axis_dir: the pull direction of an axis handle. plane_normal: the drag
// plane of a plane handle is spanned by its two basis vectors, so its normal
// is the pair's THIRD basis vector. Both expect their own handle kind.
bool gizmo_handle_is_axis(GizmoHandle handle);
simd_float3 gizmo_axis_dir(const GizmoFrame& frame, GizmoHandle handle);
simd_float3 gizmo_plane_normal(const GizmoFrame& frame, GizmoHandle handle);

} // namespace sq
