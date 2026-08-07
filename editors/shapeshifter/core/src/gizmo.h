#pragma once
#include <simd/simd.h>
#include <optional>

#include <shapeshifter/ShapeshifterCore.h>   // GizmoHandle (interop enum)
#include "camera.h"                          // Camera, Ray
#include "frame.h"                           // NodePlacement

namespace sq {

struct Node;

// Gizmo apparent size: half_extent as a fraction of viewport height at the
// frame origin's depth (screen-constant, R1 user ruling — a world-constant
// gizmo would shrink to ~3% of the view at the camera's 90-unit radius
// clamp and swallow the screen fully zoomed in). 0.24 = the R2 "2x larger"
// user ruling over the original 0.12.
inline constexpr float kGizmoScreenFraction = 0.24f;

// Plane-handle patch bounds, as fractions of half_extent: the patch is the
// square origin + x*grid_u + y*grid_v with x, y in [inner*he, outer*he].
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
// (grid_normal, grid_u, grid_v) is the GRID plane, orthonormal and right-handed
// like the frame proper, and deliberately independent of (n, u, v). For an
// attached node the two coincide: the tangent plane is both the surface you
// slide on and the u-v plane. For a free node the grid is world-horizontal
// (grid_normal = +Y) -- a local echo of the ground plate, which is more use than
// the vertical wall the u-v plane would give. Scale draws no grid and leaves
// these at (n, u, v).
//
// This plane is BOTH the grid and the single plane handle: the grid is the
// affordance for the patch drawn inside it, and the patch is the grabbable part
// of the grid. They were briefly separate -- three basis-pair patches against a
// world-horizontal grid -- which meant a free node's grid advertised XZ while
// its plane handle dragged in XY. One plane, one meaning.
struct GizmoFrame {
    simd_float3 origin;
    simd_float3 n, u, v;
    simd_float3 grid_normal, grid_u, grid_v;
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
//   0.24 .. 0.50   Placement: the move plane patch (off-axis, so never on a shaft)
//   0.885 .. 0.995 Shape: scale box tips (the box's own extent -- there is no shaft)
//   1.15           Placement: rotation rings (a radius, not a range)
//
// Scale is a BOX ON THE AXIS RAY with no shaft behind it, and that is a
// declutter decision, not a styling one. A shaft running 0.70..0.94 put a second
// bar of the same hue on the same line as the move axis, distinguishable only by
// how far out it started -- six coloured bars on three lines. One line per axis
// ending in a dot, with a box floating beyond it, is the same information
// without the duplication (and is what Blender's combined gizmo does). The band
// narrows to the box because drawn = hit: the pickable segment is exactly the
// box, and the ~14 pt tolerance around it is what makes a small target grabbable.
//
// These are the DRAWN extents, and the grab tolerances (8-14 pts) that surround
// them are in points, so the two cannot be compared here. But the arithmetic is
// worth doing once: he is kGizmoScreenFraction of the viewport height, so on a
// 500 pt viewport the 0.285 he between the move band and the scale box is around
// 34 pts, comfortably clear of the 8 + 14 pts of tolerance that meet there. The
// axis bands genuinely do not overlap now, which they did not before the shaft
// came out. pick_gizmos still resolves the order explicitly rather than trusting
// that -- the two gizmos stop sharing an origin the moment the pair splits, and
// no rule about world-space radii can settle a screen-space contest.
//
// The 0.155 he between the scale box and the rings is the tighter gap: below
// roughly a 450 pt viewport the scale tolerance reaches the ring, and since
// scale is resolved first, a ring grab near one of the three axis directions
// goes to scale instead. Accepted rather than fixed -- the fix is a bigger
// gizmo, and it only bites in a window small enough that the whole manipulator
// already fills half the viewport.
inline constexpr float kMoveAxisOuterFrac  = 0.60f;  // pick clamp
inline constexpr float kMoveAxisShaftFrac  = 0.55f;  // drawn shaft end, and where its tip dot sits
// Box tips, larger than the move gizmo's terminator dots: box-tipped axes are
// the universal scale convention, and with no shaft to carry the reading, the
// size difference is the whole of what tells the two gizmos apart at a glance.
inline constexpr float kGizmoScaleTipHalfSizeFrac = 0.055f;
inline constexpr float kScaleTipCenterFrac = 0.94f;  // where the box sits on the axis ray
// The pickable band IS the box, derived rather than written out so a restyle of
// the box cannot leave its hit region behind.
inline constexpr float kScaleAxisInnerFrac = kScaleTipCenterFrac - kGizmoScaleTipHalfSizeFrac;
inline constexpr float kScaleAxisOuterFrac = kScaleTipCenterFrac + kGizmoScaleTipHalfSizeFrac;
inline constexpr float kRotateRingFrac     = 1.15f;  // ring radius, drawn and picked
// Ordering asserts, on the DRAWN extents only -- the grab tolerances are in
// points and cannot be compared against fractions of he here (see the note
// above). They pin the layout's intent, not a guarantee of non-overlap.
static_assert(kMoveAxisOuterFrac < kScaleAxisInnerFrac,
              "move axes must be drawn inboard of the scale boxes: they share three lines");
static_assert(kScaleAxisOuterFrac < kRotateRingFrac,
              "the rings must be drawn outboard of every axis");
static_assert(kMoveAxisShaftFrac <= kMoveAxisOuterFrac,
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
// Three box tips on the axis rays plus a centre box for uniform scale. No
// shafts (see the band table), no plane handle and no grid: the grid plane
// belongs to the Placement gizmo, and scale has no drag plane of its own.

// The centre (uniform) handle's drawn half-size. With the boxes way out at
// kScaleTipCenterFrac, the centre owns the disc around the origin outright, so
// "aim at the middle" can never be answered by an axis.
inline constexpr float kGizmoUniformHalfSizeFrac = 0.09f;

// Scale's axis grab tolerance, deliberately wider than Move's
// kAxisPickTolerancePts. With no plane handle and no grid, the scale gizmo's
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

// The grid plane's in-plane basis, given that plane's normal and the frame's own
// axes. grid_u is whichever of (u, v, n) leans least on grid_normal, projected
// into the plane and renormalized; grid_v = cross(grid_normal, grid_u).
//
// NOT tangent_basis(grid_normal). That would derive the basis from the world
// alone and land the plane patch -- which is drawn in the +grid_u/+grid_v
// quadrant -- in whatever direction the {0,1,0}/{1,0,0} reference vector happens
// to point, typically nowhere near a drawn axis. Deriving it from the frame
// instead keeps the patch beside an axis handle the user can already see, and
// reproduces (u, v) EXACTLY when the grid plane is the u-v plane: there u and v
// both have zero component along n, u wins the tie by declaration order, and
// cross(n, u) is v by tangent_basis's own definition.
//
// Always well-conditioned: the least-aligned of three orthonormal vectors can
// have at most 1/sqrt(3) of itself along any unit normal, so the projection
// being renormalized is never shorter than sqrt(2/3).
void grid_basis(simd_float3 grid_normal, simd_float3 u, simd_float3 v, simd_float3 n,
                simd_float3& grid_u, simd_float3& grid_v);

// Takes a resolved placement, NOT a Node -- and needs nothing else from one,
// which is the point. Everything this used to read off a node (its position,
// its rotation, whether it was snapped and where) is now the document's answer
// to give, so the gizmo cannot disagree with what is rendered no matter how the
// storage behind that answer changes.
GizmoFrame gizmo_frame_for_node(const NodePlacement& placement, const Camera& camera,
                                GizmoSlot slot);

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
// distance vs the pts tolerance above), and the plane patch
// origin + x*grid_u + y*grid_v with x, y in [kGizmoPatchInner*he,
// kGizmoPatchOuter*he] (ray_plane + bounds). All named rather than spelled out
// on purpose -- this comment once restated the bounds as literals and went stale
// the moment they moved, which is the exact failure those constants exist to
// prevent. Any axis hit beats the plane; among axes smallest distance wins
// (ties: smaller ray-t, then declaration order).
//
// `slot` selects the handle set, and is REQUIRED rather than defaulted: which
// manipulator is being hit-tested is exactly the kind of thing that must not be
// decided silently at a call site.
//
// Placement: axes over 0..kMoveAxisOuterFrac*he, then the rings and the plane
// patch. Shape: the uniform centre first (it owns the disc the boxes are pulled
// out of), then the axis boxes over kScaleAxisInnerFrac..kScaleAxisOuterFrac at
// the wider scale tolerance, and no plane.
GizmoHandle pick_gizmo_handle(const GizmoFrame& frame, const Ray& ray,
                              float fov_y_radians, float viewport_h_pts, GizmoSlot slot);

// Hit-tests BOTH gizmos and resolves the winner. This is what input should
// call; the single-slot version above exists for the pieces it is built from
// and for tests.
//
// Resolution runs innermost band outwards -- Shape's uniform centre, then
// Placement's move axes, then Shape's scale boxes, then Placement's rings and
// plane patch -- with ONE distance comparison at the front: the uniform centre
// only beats a move axis if it is genuinely closer to the ray.
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
// axis_dir: the pull direction of an axis handle, and expects one. plane_normal
// is grid_normal: there is exactly one plane handle, so the answer no longer
// depends on which one -- the parameter stays to keep the call sites reading as
// a lookup, and to say out loud that this is only meaningful for GizmoHandle::Plane.
bool gizmo_handle_is_axis(GizmoHandle handle);
simd_float3 gizmo_axis_dir(const GizmoFrame& frame, GizmoHandle handle);
simd_float3 gizmo_plane_normal(const GizmoFrame& frame, GizmoHandle handle);

} // namespace sq
