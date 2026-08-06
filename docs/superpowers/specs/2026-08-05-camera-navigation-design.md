# Camera navigation rework — design

Approved 2026-08-05. Replaces the modal camera with an always-on,
cursor-anchored navigation model: the pivot is resolved from what is under the
cursor at gesture start, pan runs on world axes at hit depth, and dolly zooms
toward the cursor. Camera mode disappears; `Select` merges into `Modify`,
leaving two modes (Edit, Spawn).

Motivation: the app targets **stylized character/creature models and procedural
face generation** — work that means constantly reorienting around small local
features close to the origin. Three properties of the previous model fought
that directly:

- **Pan had no world reference.** `pan_view` moved `target` along the camera's
  `right`/`up`. Within a gesture the plane is fixed (yaw/pitch don't change
  mid-drag), so the real problem was *across* gestures: after every orbit,
  "drag left" meant a different world direction, and muscle memory never formed.
- **Pan's scale was tied to `radius`, not to what you grabbed.**
  `2·radius·tan(fov/2)/h` is correct only for content at orbit-target depth.
  Zoomed in on a brow ridge while the target sat at the head's centre, the drag
  overshot.
- **Pan drifted the pivot.** Panning to bring an ear into view left the pivot at
  an arbitrary point behind the ear at the old radius; every later orbit swung
  around that drifted point. "Orbit a local feature" was never what actually
  happened. This is why the model felt worse than the sum of the first two.

A fourth, structural: reaching the camera required switching to mode 4. No
shipping editor does this — a survey of ZBrush, Nomad/Forger, Fusion 360,
Onshape, Blender and Maya found camera navigation is always either a modifier
chord or what empty space does, never a mode you travel to.

## User rulings

Settled by a question-at-a-time design pass, with prior art surveyed per
decision:

- **Input target: trackpad now, Pencil/tablet soon.** The binding constraint —
  every camera verb needs a single-pointer path, so nothing essential may live
  only on pinch or two-finger scroll. This is the constraint ZBrush designed
  around (LMB-drag orbit, Alt+drag pan, all one pointer).
- **Pivot: auto-pivot under the cursor.** At gesture start, raycast the scene;
  the pivot becomes the hit point. Fusion/Onshape/Nomad model. Rejected:
  selection-anchored (features often aren't nodes), explicit sticky pivot (an
  extra deliberate act per reorientation), and auto-plus-override (more state to
  explain than the gain justified).
- **Pan: world axes with hit-depth scale.** Horizontal → the camera's
  ground-projected right; vertical → world ±Y; magnitude from the eye→focus
  distance. Rejected: hit-depth view-plane grab (fixes scale but leaves
  direction camera-relative), ground-plane grab (degenerates near the horizon,
  no vertical motion), and world axes at target depth (keeps the scale error).
  Characters are Y-dominant subjects — "travel up the body" is frequent enough
  that pure vertical wins over literal 1:1 pixel tracking.
- **Modes: Edit + Spawn.** Camera mode is deleted; `Modify`-with-nothing-selected
  already behaved exactly like `Select`, so the two collapse. Rejected: keeping
  a camera lock, and binding camera to a held modifier only.
- **Gestures: scroll pans, pinch dollies.** Two-finger scroll takes its
  Mac-native content-scroll meaning now that drag owns orbit. One gesture per
  verb, nothing bound twice. Rejected: keeping scroll on orbit (orbit bound
  twice, pan buried under ⇧+scroll), and CAD-style scroll-zoom.
- **Dolly: zoom toward the cursor.** Rejected: re-pivot-then-scale-radius, and
  plain radius scaling.
- **Feedback: predictive pivot dot on hover, only.** Rejected: pivot ground drop
  line, corner axis gizmo/ViewCube (still sequenced separately), and a pan-axis
  ghost during ⌥drag.
- **Scale gets a real gizmo**, so one law governs the whole viewport: grab a
  handle or it's camera. Rejected: letting an armed Scale tool swallow all
  drags, and a proximity radius around the node (an invisible boundary — the
  exact thing the "off-handle clicks are inert" ruling exists to avoid).

Out of scope: acceleration structures for picking; the corner axis
gizmo/ViewCube; morph/animation-aware pivot invalidation (SDF nodes are static
by project constraint).

## 1. One primitive: the focus point

All three verbs need the same answer — *what world point is under this cursor?*
Resolved **once per gesture**, never per event, in new
`core/src/navigation.{h,cpp}`:

```
resolve_focus(scene, ray, fallback_target)
  1. raycast_scene hit                          -> FocusSource::Scene
  2. y = 0 intersection, 0 < t < Camera::kFar   -> FocusSource::Ground
  3. plane through target, ⊥ ray                -> FocusSource::TargetPlane
  4. fallback_target itself                     (finite by construction)
```

Reuses `raycast_scene` and `ray_plane` (`core/src/picking.h`). The `kFar` bound
on rung 2 matters: a near-horizon ray otherwise returns a point 10⁶ units out.

This lives in `navigation`, not `CameraController`, because it needs
`SceneDocument` and the controller should stay scene-free. `CoreTests` links
`ShapeshifterCore.a`, so it is unit-testable with no Metal device.

**`resolve_focus` must never return a non-finite point** — a non-finite
candidate falls through to the next rung. This is a new severity level in this
codebase: unlike a failed pick, which merely fails to select, a NaN pivot
corrupts camera state with no user-visible recovery.

## 2. The three verbs

**Orbit.** `set_pivot_preserving_eye(focus)` moves the orbit centre and nothing
else. Re-pivoting never moves the camera; it only redefines what a rotation
does, which is what makes auto-pivot safe to apply on every gesture.

> **Corrected during implementation.** This section originally said the pivot
> was recomputed as `yaw/pitch/radius` from the unchanged eye. That is wrong,
> and pinned by a test that failed before the fix: `CameraController` conflated
> the orbit pivot with the look-at target, and `to_camera()` derives the view
> direction from that target — so re-pivoting to an off-axis point **swung the
> camera to centre it**, jolting the image ~30 px at every press. Since aiming
> at a feature is by definition aiming off-centre, this would have broken the
> headline behaviour outright. The parameterization simply cannot express an
> off-axis pivot.
>
> The pivot is now separate state from the look-at target, and `orbit()`
> rotates the arm (pivot → eye) and the view direction by the *same* angular
> deltas. When the two coincide the angle pairs are equal by construction, so
> this collapses exactly onto the previous behaviour — a compatible extension,
> not a rewrite. Two consequences worth recording:
>
> - The pitch delta is *shrunk* (`clamp_pitch_delta`) rather than recomputed as
>   `clamp(angle+delta) − angle`. The latter returns a nonzero correction for a
>   zero request whenever an angle sits an ulp outside the limit, so a gesture
>   asking for nothing would nudge the camera and report that it moved.
> - The look-at target is re-derived only when the pivot genuinely differs from
>   it; doing it unconditionally fed a float ulp of drift in on every call.
>
> The pivot marker now draws at the pivot rather than at `camera.target`, since
> those are different points and showing the rotation centre is its whole job.

**Pan.** Two findings shaped this:

- **`right` is already horizontal.** `right = normalize(cross(f, {0,1,0}))` has
  `y == 0` for every pose in this rig, so horizontal pan was world-locked all
  along. The entire direction fix is one term — camera `up` becomes `pan_up`.
- Magnitude changes from `2·radius·tan(fov/2)/h` to `2·‖focus−eye‖·tan(fov/2)/h`.

**Steep-pitch degeneracy.** World-`up` panning has a component along the view
axis — that is the point — but at `kPitchLimit` (≈87°) world Y is only 3° off
the view axis, so a vertical drag would move the target almost straight at the
camera and the screen would barely change. Two pure, separately testable
helpers handle it:

- `pan_up_blend(pitch) = smoothstep(0.77, 0.97, |sin pitch|)` — the amount of
  camera-up mixed in. **0** below ~50° (pure world Y, where nearly all character
  work happens), easing to 1 by ~76°.
- `pan_vertical_gain(pitch) = mix(1/max(|cos pitch|, 0.6), 1.0, blend)` —
  world-Y motion of length `L` only shows `L·cos(pitch)` on screen. Since it is
  a mix of `raw ≤ 1/0.6` with 1, the provable bound is **1/kPanGainCosFloor ≈
  1.67** (the actual peak is ≈1.63, near `|sin p| = 0.8`). **Equals 1.0 at both
  pitch 0 and full blend**; it does not hold its maximum out to the limit.

  The floor and the blend **overlap** rather than handing off: the floor starts
  binding at `|sin p| > 0.8`, where the blend has only reached ≈0.06 and does
  not finish until 0.97. Harmless — the bound above holds throughout — but an
  earlier draft of this spec and of the header comment both claimed the blend
  retired the correction *before* the floor could bind, which is false for these
  constants.

**Dolly.** With the pivot on a surface, scaling `radius` is the wrong operation.
The right one is a rigid translation of the whole orbit rig along the eye→focus
ray:

```
eye₁    = P + f·(eye₀ − P),   f = exp(−dy·kDollySens)
target₁ = target₀ + (eye₁ − eye₀)
```

`radius` and orientation are untouched, so `P` provably projects to the same
pixel — zoom-to-cursor is exact, not approximate. The clamp is on the **eye→focus
distance**, not radius, because with a surface pivot radius is no longer "how
close am I to the thing".

Dolly then leaves `target` sitting `radius` units ahead of the eye, possibly
well past the model — and the next orbit's auto-pivot resets it to the real
surface. The two features cover each other's weak spot, which is why neither
needs a correction term of its own.

Note the approach is *already* asymptotic: `f` is exponential in drag distance,
so eye→focus distance decays exponentially toward `P`. `kFocusDistMin` is a
backstop a normal gesture never reaches, not the primary feel; layering a second
decay curve on top would only make dolly sluggish through its useful range.

## 3. Constant retunes

| Constant | From | To | Why |
|---|---|---|---|
| `CameraController::kRadiusMin` | 0.5 | 0.15 | With the pivot *on* the skin, this is a hard wall between you and the surface — unusable at nostril/eyelid scale on a 1–2 unit head. |
| `Camera::kNear` | 0.1 | 0.05 | So geometry in front of the pivot isn't clipped at close range. Trades depth precision; the raymarch writes true per-pixel depth. |
| `kFocusDistMin` | — | 0.15 | New; dolly's clamp. |
| `kDragThresholdPts` | — | 4.0 | New; click vs drag. AppKit exposes no public system drag threshold (unlike UIKit), so this is ours to own — one named constant, sized per input source once the Pencil path lands, since a stylus jitters far more than a trackpad tap. |

The first two are the ones most likely to need retuning by eye.

## 4. Scale gizmo

So Scale obeys the same "grab a handle or it's camera" law as Move, and
`syncGizmo`'s hide-the-gizmo hack goes away.

**Frame.** `gizmo_frame_for_node` gains a kind. Move keeps the
`drag_plane_for_node`-derived tangent frame; **Scale uses the node's local
axes** — currently world X/Y/Z, since `Node::rotation` stays identity. A
camera-facing basis would be meaningless for scale because it maps to no node
axis. The existing `kColorAxisU/V/N` already alias `kGroundAxisX/Y/Z`, so scale
axes read as world axes for free.

**Handles.** Three axis handles (→ `scale.x/y/z`) plus a centre uniform handle.
No plane patches and **no grid** — the grid is a drag-plane affordance and scale
has no drag plane. `GizmoHandle::Uniform = 7` joins the interop enum;
`gizmo_handle_is_axis` returns false for it.

**Grab tolerance.** With no patches and no grid, Scale's interactable footprint
is much smaller than Move's, and a near-miss now spins the camera rather than
doing nothing. `kAxisPickTolerancePts = 8.0f` already provides screen-constant
padding; Scale raises it (~14 pt) and the uniform handle gets its own tolerance
of the same family, both as named constants next to `kGizmoPatchInner/Outer` so
"drawn = hit" stays inspectable. The existing hover highlight
(`updateGizmoHover` on mouse-moved) is the pre-press signal that a handle is
live — that is what keeps this from needing an invisible dead zone.

**Drag math.** Axis: `factor = s / start_s` from the already-tested
`ray_axis_param` (Maya's formulation). Guard `|start_s| > 0.15·he` so grabbing
at the origin can't divide by ~0, and clamp `factor` positive so dragging
through the origin can't flip the sign. Uniform: reuse today's
`exp(−Δy · 0.005)` — **the centre handle is exactly today's scale gesture**, now
anchored to a real affordance, so only the three axis handles are new math.

**One drag path.** `beginScale`/`updateScale`/`endScale` disappear;
`beginDrag`/`updateDrag`/`endDrag` dispatch on the gizmo kind, and core captures
the start position so it owns the cumulative-from-start logic Swift was doing
via `scaleDragStartY`.

## 5. Interop

Removed: `cameraOrbit`, `cameraZoom`, `cameraPan`, `beginScale`, `updateScale`,
`endScale`. Added:

```cpp
enum class CameraGesture : int32_t { Orbit = 0, Pan = 1, Dolly = 2 };

void  beginCameraGesture(CameraGesture kind, float anchorX, float anchorY);
void  updateCameraGesture(float dxTotal, float dyTotal);   // CUMULATIVE from start
void  endCameraGesture();
float dollyPointsForMagnification(float cumulativeMagnification) const;
void  frameSelected();
void  setGizmoKind(GizmoKind kind);
void  updateFocusPreview(float x, float y);
void  clearFocusPreview();
```

`updateCameraGesture` taking **cumulative** displacement (matching the existing
`updateScale` convention) makes it idempotent and immune to event coalescing.
`dollyPointsForMagnification` keeps all sensitivity policy in core rather than
leaking a constant into Swift.

`frameSelected` re-pivots to the selected node and sets radius from
`node_bounding_radius` with margin, **orientation unchanged** — framing should
re-centre and re-range, not reorient. Radius is
`max(node_bounding_radius · margin, kMinFrameRadius)`: without the floor,
framing a nostril-scale node fills the screen with featureless surface and
strands you with no context. A single floor constant achieves what scaling
padding by relative volume would, without needing a parent-volume notion the
scene document doesn't have.

## 6. Predictive pivot dot

`append_focus_dot` (one camera-facing quad, 6 verts) plus renderer
`set_focus_preview`/`hide_focus_preview`, drawn last and depth-ignored like the
pivot crosshair. Quiet: ~3 pt radius, alpha ~0.35. Lights **only** on
`FocusSource::Scene` — a dot floating in space on a ground or target-plane
fallback is noise, not information.

**Known divergence, accepted.** `raycast_scene` intersects the *analytic
primitives* and ignores CSG: a `Subtract` node is pickable as a solid, and a
region carved away by one still reports a hit (`picking_tests.cpp` pins this
deliberately). So the dot can land on a surface the raymarch does not draw.
This is pre-existing behaviour for click-picking, but a continuously-visible dot
makes it continuously visible. Accepted for now; the alternative is sampling the
rendered depth buffer, which needs a GPU→CPU readback this renderer has no sync
primitives for (see the orientation-aids spec's deferred-pooling note) and would
move gesture math out of the Metal-device-free unit-testable core. If it proves
distracting, the fix is a CSG-aware CPU evaluator, not a depth readback.

## 7. App layer

`EditorMode` becomes `.edit, .spawn` (keys 1/2, plus `f` for frame-selection).
The merge is mostly deletion: the `.select`/`.modify` split in `handleMouseDown`
and the mid-gesture abort branch in `setMode` both collapse. `deleteSelected`
stops switching to camera mode and just clears the selection.

Pointer state machine:

```
pending(down, modifiers) -> below threshold, undecided
manipulating             -> gizmo drag (move or scale)
camera                   -> gesture running in core
```

- **mouseDown**: try `editor.beginDrag(x,y)`; success → `manipulating`, else
  `pending`. The existing "off-handle clicks are inert" ruling — `beginDrag`
  already returns `false` off-handle — makes this seam correct as it stands.
- **mouseDragged**: `pending` + moved > `kDragThresholdPts` → pick the gesture
  from the modifiers captured at mouse-down (`⌥` pan, `⌘` dolly, else orbit) and
  `beginCameraGesture` at the **mouse-down point**. That anchor is what makes
  "point at the nostril and drag" work.
- **mouseUp**: `pending` → the click action (pick+select in `.edit`, spawn in
  `.spawn`).

**Phased scroll and pinch.** `NSEvent.phase` delivers `.began/.changed/.ended`
for precise trackpad scrolling and for `magnify(with:)`, so scroll→pan and
pinch→dolly map cleanly onto begin/update/end, accumulating deltas for the
cumulative update. Legacy wheels report `.none`; Swift synthesizes
begin+update+end per event.

> This corrects a claim in the viewport-orientation-aids spec — "scroll and
> magnify are discrete event streams with no end event, so there is no single
> place a 'gesture ended' signal could come from". True for wheels, not for
> trackpads. The pivot marker's timestamp-based fade still works and still
> covers the legacy path, so this is additive, not a rework of that feature.

`mouseMoved` in `.edit` also drives `updateFocusPreview`.

## 8. Testing

`raycast_scene` goes from "runs on click" to "runs on every gesture start and
every mouse-move", so it is validated first, before anything depends on it.
Note it does **not** raymarch — `picking.cpp` includes only `scene.h` and solves
analytic ray-primitive intersections (`ray_unit_sphere` is a quadratic,
`ray_unit_cube` a slab test) per node in local space; sphere tracing lives only
in `shaders/raymarch.metal` on the GPU. Cost is O(nodes) at a few dozen flops
each plus one `simd_inverse`. Acceleration structures are out of scope.

New cases in `picking_tests.cpp`, chosen for what is newly load-bearing rather
than what is merely untested:

1. **Oblique hit on a non-uniformly scaled sphere.** The existing non-uniform
   case hits along the scale axis, so its expected normal is trivially `+x` and
   the `transpose(inverse(M))` path is never really exercised. Pinned against
   the closed form: for `(x/a)² + (y/b)² + (z/c)² = r²`, the world normal at `P`
   is `normalize((Pₓ/a², P_y/b², P_z/c²))`.
2. **`world_t` is a true distance.** `raycast_scene` computes
   `world_t = dot(world_point − origin, dir)`, valid only because `dir` is unit.
   Also: nearest-wins ordering across nodes with very different scales, where
   the local `t`s are in different parameterizations and only the world `t`
   comparison is meaningful. This is the subtle bug this code is most exposed to.
3. **Ray origin inside a node, at scene level.** Newly reachable — dolly-to-
   cursor can put the eye inside geometry. Must return the exit face at `t > 0`.
4. **Round-trip against the camera.**
   `ray_through_view_point → raycast_scene → project` must land back where it
   started, over a grid of view points. Cross-validates three functions with no
   hand-computed literals, catching sign/aspect/flip errors per-function tests
   miss.
5. **Extreme scale.** `[0.05, 50]` (the `updateScale` clamp) raycasts finitely
   and still yields a unit normal.

New `navigation_tests.cpp`: focus chain order; ground rejected beyond `kFar`;
`resolve_focus` never non-finite, including a ray exactly parallel to y=0 and a
camera sitting exactly at y=0 (the same NaN class the ground-grid shader had to
guard against — orientation-aids hazard #2); `node_bounding_radius` per shape
under non-uniform scale.

`camera_controller_tests.cpp` gains: `set_pivot_preserving_eye` leaves the eye
where it was across a spread of pivots, rejects non-finite, clamps distance;
`pan_world` gives pure world −Y at low pitch and `right.y == 0` across poses;
blend/gain continuity and boundedness at both endpoints. `dolly_toward` asserts
the **property** — `project(focus)` unchanged after the dolly — rather than the
algebra, because that property is the entire claim.

Editor-level integration (`Editor::create()` works without a Metal layer, as
`picking_tests.cpp` already relies on): an orbit gesture on a spawned node
re-pivots to its surface and does **not** move the eye; cumulative-update
idempotence (one `update(100,0)` equals ten calls ending at `(100,0)`); an
empty-scene gesture falls through the chain and stays finite.

`gizmo_tests.cpp`, `lines_tests.cpp` and `drag_tests.cpp` change by construction
for the new handle set, plus axis-scale factor cases covering both guards.

App layer has no Swift tests (house convention); verified by running.

## 9. Implementation hazards

1. **NaN reaches further than it used to.** Pre-rework, a bad raycast produced a
   failed selection. Now it can land in the pivot and permanently break the
   camera with no user-visible recovery. Every path from `resolve_focus` into
   `CameraController` rejects non-finite input rather than trusting upstream.
2. **`ray_axis_param` on the infinite line.** Dragging a scale axis handle past
   the frame origin flips `s`'s sign, and an unguarded `s / start_s` would flip
   the node inside out. Guarded at both ends (minimum `|start_s|`, positive
   factor clamp).
3. **Near-plane change is not free.** `kNear` 0.1 → 0.05 with `kFar` 100 widens
   the depth ratio to 2000:1. The raymarch writes true per-pixel depth into a
   `Depth32Float` target, so this should hold, but z-fighting between the ground
   plate and resting geometry is the thing to watch when retuning by eye.
4. **Modifier capture timing.** Modifiers are read at mouse-down and held for
   the gesture. Reading them per-event would let a mid-drag ⌥ press silently
   switch orbit into pan.
