# Move gizmo with axis/plane handles — design

Approved 2026-07-31. Modify-mode move upgrade: the gizmo draws the full
tangent-basis axes and plane handles; the user pulls the node along an axis
(1-DOF) or a plane (2-DOF) by clicking directly on a handle and dragging.
Handles highlight on hover and while dragging. Goal: more control when
moving. Scaling is explicitly out of scope.

User rulings: unsnapped nodes keep the camera-facing basis (today's
convention — axes swim as the camera orbits); all three plane handles
(u–v, u–n, v–n), not just the tangent plane; hover + active highlight
(new mouse-moved plumbing accepted); clicking off any handle does
**nothing** — a deliberate behavior change from today's click-anywhere
free drag, so existing `drag_tests.cpp` free-drag cases get reworked.

AMENDMENT (R1, 2026-07-31, external plan review + user rulings): the
gizmo is **screen-constant sized** (whole gizmo, grid included):
`half_extent = kGizmoScreenFraction (0.12) · length(origin − eye) ·
2·tan(fov_y/2)`, replacing the world-constant `kGizmoHalfExtent`;
`tangent_basis` gains a degenerate-normal fallback (‖n‖ ≈ 0 → {0,1,0});
axes seen nearly end-on (`|dot(ray, axis)| > 0.995`) are unpickable —
this, not the solver's 1e-6 denominator, is what protects axis drags
from parameter spikes; axis tie-break is distance, then nearest ray-t,
then declaration order; `deleteSelectedNode` clears hover (it bypasses
`select()`). Rejected in the same review, with verification: mouse-move
throttling (pick is ~6 analytic tests), a resize-mid-drag guard
(updateDrag already reads current viewport dims), and a behind-camera
code guard (forward-clamped world-space math already cannot pick
behind-camera handles; pinned by test only). The plan's "R1 review
amendments" section carries the details.

AMENDMENT (R2, 2026-07-31, user visual rulings after hands-on): gizmo is
2x larger (`kGizmoScreenFraction` 0.12 → 0.24); handle lines are THICK —
expanded CPU-side into camera-facing quads (Metal has no line width) of
half-width `kGizmoHandleHalfWidthFrac (0.02) · he`, drawn as triangles in
a second gizmo draw while the grid stays thin lines; handles are COLORED:
axes u/v/n = red/green/blue, planes = the additive mix of their two axes
(uv yellow "110", un magenta, vn cyan). Hover/active hot color moved from
amber to WHITE — amber collides with the yellow uv plane. §6's
`append_move_gizmo` split into `append_move_gizmo_grid` (lines) +
`append_move_gizmo_handles` (triangles, takes the eye for the facing
expansion); `Renderer::set_gizmo(frame, highlighted, eye)`.

AMENDMENT (R3, 2026-07-31, user rulings): gizmo axes draw the POSITIVE
half only (0..+he) and the pickable segment clamps to match (drawn = hit
preserved; the drag solver still runs the full line, so a grabbed axis
drags both ways). New camera-pivot marker at the orbit target, drawn
every frame in every mode, ALWAYS last: a world-axis-aligned "spiked
cube" (12 wireframe edges, corners ± he, plus one spike per face from
the wall center out to 2he), screen-constant (`kPivotScreenFraction`
0.025 of viewport height at target depth), 2px-thick camera-facing
quads (`kPivotLineHalfWidthPts` 1pt half-width), mid gray — opaque
where it beats the scene depth (read-only Less pass), alpha 0.25 where
occluded (read-only Greater pass; disjoint, no double blend). Geometry:
`append_spiked_cube` in lines.h; `Renderer::set_pivot_gizmo`; two new
read-only depth-stencil states.

## 1. Gizmo frame — new `core/src/gizmo.h/.cpp`

`GizmoFrame { simd_float3 origin; simd_float3 n, u, v; float half_extent; }`
built by `gizmo_frame_for_node(const Node&, simd_float3 camera_forward)`:

- origin/n from the existing `drag_plane_for_node` (snapped →
  {snap_point, snap_normal}; unsnapped → {position, −camera_forward}).
- u/v from the tangent-basis formula currently private to `lines.cpp`
  (`u = normalize(cross(n, |n.y| < 0.99 ? {0,1,0} : {1,0,0})), v =
  cross(n, u)`). The formula moves into the gizmo module as
  `tangent_basis(n) → (u, v)` and `lines.cpp` consumes it — one source of
  truth so rendering and hit-testing cannot drift.

`enum class GizmoHandle { None, AxisU, AxisV, AxisN, PlaneUV, PlaneUN,
PlaneVN }` — interop-friendly (int32 underlying).

## 2. Geometry (drawn = hit)

- Three full axes through the origin, −he..+he along u, v, and n
  (he = `kGizmoHalfExtent`, world-constant as today). The n axis grows
  from today's half-length stub to a full axis — it is the new degree of
  freedom (lift off the surface / pull along the view direction).
- Three plane handles: positive-quadrant square outlines, the patch
  `origin + x·e₁ + y·e₂` with `x, y ∈ [0.3·he, 0.6·he]` for each basis
  pair (u–v, u–n, v–n). The patch is both the drawn outline and the hit
  region.
- The existing grid stays as-is — decoration on the u–v plane, not a
  handle.

## 3. Hit-testing — `pick_gizmo_handle(frame, ray, …) → GizmoHandle`

- Axes: world-space ray-to-segment closest distance against a
  screen-constant tolerance (~8 pts) converted to world units at the
  closest point's depth via the same `2·d·tan(fov_y/2)/viewport_h_pts`
  scale `pan_view` uses. No projection edge cases (nothing to clip
  behind the camera).
- Planes: `ray_plane` intersect + basis-coordinate bounds check against
  the quad patch.
- Priority: any axis hit beats any plane hit (axes are thin targets);
  among axes the smallest ray distance wins; among plane hits the
  nearest ray-t wins. No hit → `GizmoHandle::None`.

## 4. Constrained drag — Editor

- `beginDrag(x, y)` becomes: build the frame, `pick_gizmo_handle`; **no
  handle → no drag activates**. Interop signature changes to `bool
  beginDrag(float, float)` so the VM's `isDragging` mirror stays
  truthful.
- The frame is frozen at mouse-down for the whole gesture (same
  "captured NOW" rule as today's plane capture; camera gestures are
  mode-gated so the camera cannot move mid-drag anyway).
- Axis drag: closest-point-between-lines parameterization — capture s₀ =
  closest-approach parameter of the mouse ray to the axis line at begin;
  each update recomputes s and sets `position = start_pos + (s − s₀) ·
  axis`. Near-parallel ray/axis (denominator ~0) keeps the last
  position, mirroring `ray_plane`'s parallel guard.
- Plane drag: today's `ray_plane` delta path, with the plane through the
  frame origin and normal = the pair's third basis vector (PlaneUV → n,
  PlaneUN → v, PlaneVN → u).
- Snapped nodes: `snap_point` translates by the full world delta every
  update — the frame rides rigidly with the node and the `snapped` flag
  is untouched. Pulling along n intentionally lifts the node off its
  surface without re-basing mid-gesture. (Supersedes the in-plane-only
  `snap_point` comment in `updateDrag`.)

## 5. Hover — new input plumbing

- `ViewportNSView` gains an `NSTrackingArea` (`mouseMoved` +
  `mouseEnteredAndExited`, `inVisibleRect`, key-window active) and
  forwards `mouseMoved`/`mouseExited` to the VM like the existing
  handlers.
- VM: `handleMouseMoved` calls new interop `updateGizmoHover(x, y)` only
  in `.modify` with a selection; `clearGizmoHover()` on mouse-exit and on
  leaving `.modify`/losing selection.
- Editor stores the hovered handle. While a drag is active, the active
  handle is the highlight (mouseMoved does not fire mid-drag, so no
  contention).

## 6. Rendering

- `append_tangent_frame` evolves into `append_move_gizmo(out, frame,
  highlighted_handle)`: grid unchanged (`kColorGridLine`), three axes in
  `kColorGridAxis`, plane-quad outlines in a new dim plane color, and the
  highlighted handle's lines in a new `kColorGizmoHot` (amber, e.g.
  {1.0, 0.85, 0.2, 1.0}).
- `Renderer::set_gizmo` signature extends to take the frame + highlighted
  handle; `Editor::render` builds the frame per-frame (camera-dependent
  for unsnapped nodes) as it does today.
- Budget: ~80 line vertices — far under the 4 KB `setVertexBytes` limit.

## 7. Interop surface delta (`ShapeshifterCore.h`)

- `void beginDrag(float, float)` → `bool beginDrag(float, float)`
- new `void updateGizmoHover(float x, float y)`
- new `void clearGizmoHover()`
- Everything else (scale, spawn, camera, selection) untouched.

## 8. Testing

New `tests/core/gizmo_tests.cpp`:
- `tangent_basis`: orthonormality, right-handedness, continuity with the
  formula documented in `lines.h`.
- `gizmo_frame_for_node`: snapped vs unsnapped origin/n.
- `pick_gizmo_handle`: axis hit inside/outside tolerance, plane patch
  hit/miss, axis-beats-plane priority, nearest-wins ordering,
  `None` on clean miss.
- Axis solver: dragging recovers expected parameter deltas; near-parallel
  guard keeps position.
- Editor-level: `beginDrag` on a handle activates and constrains
  correctly; off-handle click leaves the node unmoved and returns false;
  hover set/clear transitions; snapped-node n-pull translates
  `snap_point` rigidly with `snapped` still true.

Existing `drag_tests.cpp`: free-drag-from-anywhere cases are reworked to
grab the u–v plane handle (or an axis) explicitly — the old behavior is
intentionally gone.

App layer (no Swift tests): verified by build + running the app — hover
highlights, axis pulls, plane pulls, off-handle inertness, and
non-regression of select/spawn/scale and camera modes.
