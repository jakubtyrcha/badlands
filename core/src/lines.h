#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <vector>
#include <shared_types.h>
#include <ground_grid.h>   // kGroundAxisX/Y/Z -- the shared world-axis palette

#include "gizmo.h"   // GizmoFrame/GizmoHandle for append_move_gizmo

namespace sq {
class SceneDocument;

inline constexpr simd_float4 kColorAdd      = {0.1f, 1.0f, 0.1f, 1.0f};
inline constexpr simd_float4 kColorSubtract = {1.0f, 0.2f, 0.15f, 1.0f};
inline constexpr simd_float4 kColorSelected = {0.6f, 0.8f, 1.0f, 1.0f};
inline constexpr simd_float4 kColorGridLine = {1.0f, 1.0f, 1.0f, 0.18f};

// Gizmo handle colors: one hue per axis, a pairwise mix per plane. Hot is
// WHITE — amber would collide with the warm uv plane.
//
// The axis colors ARE ground_grid.h's DESATURATED world-axis palette, aliased
// rather than transcribed: full-saturation primaries at this size read as a
// toy ("too cartoonish", user ruling), and sharing the constant means an axis
// handle and a world axis of the same hue cannot drift apart the first time
// either side is retuned.
inline constexpr simd_float4 kColorAxisU    = kGroundAxisX;
inline constexpr simd_float4 kColorAxisV    = kGroundAxisY;
inline constexpr simd_float4 kColorAxisN    = kGroundAxisZ;
// Hand-tuned toward each pair's additive mix rather than computed from one: a
// literal component-wise max of two desaturated colours comes out brighter
// than either parent, which would make the plane handles louder than the axes
// they belong to. The u+v / u+n / v+n RELATIONSHIP is what has to survive, not
// a formula.
inline constexpr simd_float4 kColorPlaneUV  = {0.780f, 0.640f, 0.360f, 1.0f}; // u+v
inline constexpr simd_float4 kColorPlaneUN  = {0.700f, 0.400f, 0.720f, 1.0f}; // u+n
inline constexpr simd_float4 kColorPlaneVN  = {0.400f, 0.720f, 0.760f, 1.0f}; // v+n
inline constexpr simd_float4 kColorGizmoHot = {1.0f, 1.0f, 1.0f, 1.0f};
// The scale gizmo's uniform (centre) handle. Neutral on purpose: it drives all
// three components at once, so borrowing any single axis hue would misread as
// that axis.
inline constexpr simd_float4 kColorGizmoUniform = {0.86f, 0.86f, 0.90f, 1.0f};
// The tether between a split pair of gizmos. Quiet by design: it is a
// relationship, not a handle, and nothing about it is grabbable.
inline constexpr simd_float4 kColorAnchorTether = {1.0f, 1.0f, 1.0f, 0.30f};

// Resting vs highlighted opacity. Everything used to draw at full strength
// all the time, which is most of why the gizmo shouted over the model; now
// only the hovered/active handle does. Hover feedback is therefore carried by
// brightness AND color, so it survives on any handle.
inline constexpr float kGizmoHandleRestAlpha = 0.62f;
// The rest alpha the gizmo you are NOT pointing at falls back to. Both gizmos
// are live at once and, when their anchors coincide, share an origin -- so at
// the moment you reach for a handle the other manipulator is pure competition
// for the eye. Dropping it back is what keeps a coalesced gizmo readable
// without taking any handle away.
inline constexpr float kGizmoHandleDimAlpha = 0.22f;

// Handle line half-width as a fraction of the frame's half_extent. he is
// screen-constant, so this is screen-constant thickness too (~2.6pts full
// width at the default kGizmoScreenFraction on a 500pt-tall viewport, down
// from 4.8pts before the restyle).
inline constexpr float kGizmoHandleHalfWidthFrac = 0.011f;
// The hairline outlining a plane patch's translucent fill — thinner still, so
// the patch reads as a surface rather than a second set of bars.
inline constexpr float kGizmoPatchBorderHalfWidthFrac = 0.006f;
// Each axis shaft stops just short of its band's outer edge and a camera-facing
// dot caps it; the band bounds themselves live in gizmo.h, shared with the pick.
inline constexpr float kGizmoAxisTipHalfSizeFrac = 0.030f;
// Plane patches are filled at this alpha, with their outline at full weight.
inline constexpr float kGizmoPatchFillAlpha = 0.16f;

// Gizmo grid: peak alpha at the origin, fading radially to nothing between
// these fractions of he. The hard square edge is gone (user ruling) — which
// means the fade must be evaluated per vertex, so each grid line is
// subdivided rather than drawn as a single 2-vertex segment.
inline constexpr float kGizmoGridAlpha = 0.20f;
inline constexpr float kGizmoGridFadeBegin = 0.45f;
inline constexpr float kGizmoGridFadeEnd = 0.98f;
inline constexpr int kGizmoGridSegmentsPerLine = 12;

// Camera-pivot marker: a flat white ring + crosshair, shown only while a
// camera gesture is in progress. It is feedback about a gesture, not part of
// the model, so it draws depth-ignored (never occluded) and its alpha is
// scaled by pivot_marker_alpha's fade at the call site.
inline constexpr simd_float4 kColorPivot = {1.0f, 1.0f, 1.0f, 0.85f};
inline constexpr int kPivotRingSegments = 24;
// The four ticks span this fraction of the radius, so they cross the ring
// rather than stopping at it — that crossing is what reads as a crosshair
// instead of a bare circle.
inline constexpr float kPivotTickInnerFrac = 0.45f;
inline constexpr float kPivotTickOuterFrac = 1.35f;

// World-origin marker. The pip is white; the +Y shaft's colour comes from
// ground_grid.h's kGroundAxisY -- the same header the shader reads X and Z
// from -- so the three world axes cannot drift apart across the language
// boundary.
inline constexpr simd_float4 kColorOriginPip = {1.0f, 1.0f, 1.0f, 0.9f};

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
// along BOTH u and v directions, center lines included — the axis handles
// only cover the positive halves (R3), so a skipped center would gap the
// -he..0 halves.
//
// Each line is subdivided into kGizmoGridSegmentsPerLine pieces so the
// radial alpha fade (kGizmoGridAlpha, falling to 0 between
// kGizmoGridFadeBegin*he and kGizmoGridFadeEnd*he) can be evaluated per
// vertex. Drawing a line as a single 2-vertex segment would interpolate that
// fade linearly end-to-end, which is visibly wrong for a radial falloff — the
// midpoint of a line passing near the origin would come out darker than its
// ends. 2*(divisions+1) lines * segments * 2 verts = 624 verts at
// divisions=12: past the 4KB setVertexBytes limit, hence the vertex buffer.
void append_move_gizmo_grid(std::vector<LineVertex>& out, const GizmoFrame& frame, int divisions);
//
// Handles (TRIANGLE primitives): each handle line is expanded into a
// camera-facing quad of half-width kGizmoHandleHalfWidthFrac * he (Metal has
// no line width), endpoints extended by the half-width so segments sharing a
// corner overlap instead of notching. Emits, in this order (pinned by
// lines_tests):
//  - 3 axis shafts from the origin, POSITIVE half only: 0..kGizmoAxisShaftFrac*he
//    along u/v/n (R3 user ruling; pick clamps to the full 0..he segment),
//    colors kColorAxisU/V/N — 6 verts each
//  - 3 camera-facing tip dots capping those shafts — 6 verts each
//  - 3 plane patches (uv, un, vn) over [kGizmoPatchInner, kGizmoPatchOuter]^2
//    in each basis pair: a translucent fill quad (6 verts) then a 4-segment
//    hairline outline (24 verts), colors kColorPlaneUV/UN/VN
//  - 1 camera-facing origin pip — 6 verts
// Handles rest at `rest_alpha` -- kGizmoHandleRestAlpha normally, or
// kGizmoHandleDimAlpha when the OTHER gizmo owns the hover, which is what keeps
// a coalesced pair readable. The `highlighted` one draws opaque in
// kColorGizmoHot instead (hover/active feedback), so pass None for whichever
// gizmo is not hovered. Plane fills and the origin pip scale with rest_alpha
// too, so a dimmed gizmo dims whole. A segment pointing straight at the eye has
// no on-screen extent and is skipped. Total: 18 + 18 + 90 + 6 = 132 verts,
// which is why these ride a vertex buffer rather than setVertexBytes.
void append_move_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                               GizmoHandle highlighted, simd_float3 eye, float rest_alpha);

// The scale gizmo's handles (TRIANGLE primitives), from the same GizmoFrame
// pick_gizmo_handle hit-tests with GizmoKind::Scale. No grid and no plane
// patches: the grid is a drag-PLANE affordance and scale has no drag plane.
//
// Emits, in this order (pinned by lines_tests):
//  - 3 axis shafts running kScaleAxisInnerFrac*he .. kGizmoAxisShaftFrac*he
//    along u/v/n, colors kColorAxisU/V/N — 6 verts each. They start OUTBOARD
//    of the centre box rather than at the origin, which is what lets the
//    uniform handle own the middle unambiguously (gizmo.h).
//  - 3 camera-facing box tips capping those shafts — 6 verts each. Larger than
//    the move gizmo's terminator dots: box-tipped axes are the scale
//    convention, and the size difference is what distinguishes the two gizmos
//    at a glance.
//  - 1 camera-facing centre box, the uniform handle — 6 verts.
// Total 42 verts. Same rest/hot/dim alpha treatment as the move gizmo.
void append_scale_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                                GizmoHandle highlighted, simd_float3 eye, float rest_alpha);

// Joins the two gizmos' anchors when they are apart: one thin segment from the
// node's attachment point to its centre. It says the two handle clusters belong
// to one node, and shows how far the detail has been lifted off the surface it
// was placed on -- the quantity a real attachment model would later make live.
// Drawn only when the pair is NOT coalesced (gizmos_coalesce), where it would
// otherwise be a zero-length segment inside the handles. 6 verts.
void append_anchor_tether(std::vector<LineVertex>& out, simd_float3 anchor, simd_float3 centre,
                          float half_width, simd_float3 eye);

// The world origin's vertical marker: the +Y axis plus a small pip at the
// origin itself. X and Z are drawn by the ground plate's fragment shader
// (they lie in the plate's plane); Y is above it, so it has to be geometry.
// Drawn in the same depth-tested pass as the plate so all three axes occlude
// consistently against the scene.
//
// `height` is world units; `half_width` and `pip_half_size` are world units
// the caller derives from a screen-constant point size at the origin's depth.
// 1 shaft segment + 1 camera-facing pip quad = 12 verts (TRIANGLE primitives).
void append_origin_marker(std::vector<LineVertex>& out, float height, float half_width,
                          float pip_half_size, simd_float3 eye);

// Predictive pivot dot: the surface point under the cursor, i.e. what the next
// orbit would rotate around. Answers "how will this drag behave?" BEFORE the
// press, which the pivot crosshair cannot — that only appears once a gesture is
// already running.
//
// Deliberately the quietest thing in the viewport: one small camera-facing
// quad, low alpha, drawn depth-ignored with the rest of the gesture chrome. It
// competes with the model for attention every time the cursor moves, so it has
// to earn its place by being nearly subliminal until you look for it.
inline constexpr simd_float4 kColorFocusPreview = {1.0f, 1.0f, 1.0f, 0.35f};
void append_focus_dot(std::vector<LineVertex>& out, simd_float3 center, float half_size,
                      simd_float3 eye, simd_float4 color);

// Camera-pivot marker (the orbit target): a flat camera-facing ring of
// `radius` with four ticks crossing it. Replaces the previous always-on
// "spiked cube" (move-gizmo spec R3), which was 3D geometry parked at the
// orbit target in every mode — permanent clutter that also read as part of
// the model. The pivot answers a question only asked mid-gesture, so this
// marker is shown only while one is running (see pivot_marker_alpha) and is
// deliberately flat: it is a screen-space annotation, not an object.
//
// Same camera-facing thick-quad expansion as the move-gizmo handles.
// kPivotRingSegments + 4 segments, 6 verts each = 168 verts.
void append_pivot_crosshair(std::vector<LineVertex>& out, simd_float3 center, float radius,
                            float half_width, simd_float3 eye, simd_float4 color);
}
