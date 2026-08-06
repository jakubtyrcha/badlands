# Gizmo frames, two anchors and rotation — design

**Corrected during implementation (2026-08-06).** Four things below did not
survive contact with the code. Recorded here rather than silently edited into
the body, because each was a real mistake in the design:

1. **§3's `grid_u, grid_v` became a single `grid_normal`.** Two vectors where
   one will do: `tangent_basis(grid_normal)` reproduces `(u, v)` exactly in the
   attached case, because it is the same call the frame used to build them.
2. **The attachment had to stop following the node.** The spec said nothing
   about `updateDrag`'s rigid `snap_point` ride, and combined with the
   spawn-placement ruling it made the Placement/Shape split unobservable —
   the two anchors would have been equal at spawn and equal forever. Settled
   by user ruling mid-implementation: the attachment is a fact about the
   surface, so it stays put and the node moves off it.
3. **§5's "resolution is by radius band, innermost first" was wrong for rings
   vs plane patches.** The bands are disjoint in radius *from the origin*,
   which says nothing about what a ray meets on its way through: a ring really
   can sit in front of a patch. Those two now resolve by nearest-along-the-ray.
   Axes still win outright, and Shape's uniform centre is still tested first.
4. **Rings resolve among themselves by 3D distance to the circle**, not by
   where the ray crosses each ring's plane. A ray aimed squarely at one ring
   crosses the other two rings' planes as it travels and can cross one at very
   nearly ring radius — picking a ring the cursor was nowhere near, and then
   reading the drag's angle in the wrong plane entirely.

Approved 2026-08-06. Replaces the single tool-modal manipulator with two live
gizmos — **Placement** (move + rotate) at the node's attachment point and
**Shape** (scale) at its centre — that merge into one when those points
coincide. Every basis becomes world- or node-locked; nothing about a gizmo
depends on the camera except its screen-constant size. `Node::rotation` stops
being decorative and becomes a real, renderable degree of freedom.

Motivation: the app targets **stylized character/creature models and procedural
face generation** — sticking details onto a surface and adjusting them in place.
Three properties of the previous manipulators fought that:

- **The move gizmo swam with the camera.** `drag_plane_for_node` returned
  `{position, −camera_forward}` for an unsnapped node, so its whole basis rotated
  as you orbited: "drag along u" meant a different world direction after every
  camera move, and muscle memory never formed. A snapped node already got a
  world-locked `{snap_point, snap_normal}` frame, so only free nodes misbehaved —
  including the first shape in any scene, whose spawn ray necessarily misses.
- **The two gizmos disagreed about where the node was.** Scale centred on
  `node.position`; Move centred on `snap_point`, which `spawn_snapped` put half a
  unit below the node's centre. Switching tools made the gizmo jump.
- **There was no rotation.** `Node::rotation` existed but was pinned to identity,
  and `SdfNode` carried no rotation field at all — the shader evaluated
  `q = p − pos`. Swivelling a detail against the surface it sits on, which is the
  core gesture of placing a nose or an eyebrow, was not expressible.

A fourth, structural: the manipulator was a *mode* reached through the radial
menu, which contradicted the one law the camera rework established — grab a
handle, or the press drives the camera. With both gizmos live there is no tool to
arm, and that law now covers the whole viewport without exception.

## User rulings

Settled by a question-at-a-time design pass, with prior art surveyed per
decision (3ds Max AutoGrid, ZBrush InsertMesh/NanoMesh/ZSpheres, Blender
transform orientations + snap-to-face + vertex parenting, Maya Make Live +
point-on-poly, Nomad's gizmo-space toggle, Fusion's feature tree):

- **Composition: two live gizmos, not tool modes.** Placement carries move and
  rotate; Shape carries scale. Both are always drawn, coalescing into one when
  their anchors are close. Rejected: keeping the radial menu tool-modal with a
  third Rotate button (never shows both anchors, so the relationship between
  attachment and centre is never visible), and showing one live gizmo plus a
  passive dot at the other anchor (advertises a reference point you cannot grab).
- **Anchors: Placement at the attachment, Shape at the centre.** Placement uses
  `snap_point` when snapped and `node.position` otherwise; Shape always uses
  `node.position`. A free node's two anchors are therefore the same point and it
  shows exactly one gizmo, by construction rather than by special case.
- **Free-node basis: the node's own local axes**, never camera-facing. This
  reverses the move-gizmo spec's original ruling ("axes swim as the camera
  orbits"). The always-on cursor-anchored camera is what pays for it: reorienting
  to reach a world plane is now a drag from empty space, not a mode switch.
- **Grid: a reference plane, not a drag plane.** Attached → the tangent plane
  (which is both). Free → world XZ through the anchor, a local echo of the ground
  plate. Rejected: keeping it on the frame's u–v plane (world XY for a free node —
  a vertical wall standing at the object), and re-ordering the free basis to put
  the grid on XZ (would draw a red axis pointing along world Z, which the ground
  plate itself draws blue).
- **Spawn placement: centre on the surface.** `position = hit`, replacing
  `hit + normal * 0.5`. A detail is half-embedded, which is what a union wants,
  and its two anchors coincide so a fresh detail shows one coalesced gizmo until
  it is lifted off.
- **Rotation pivots on the Placement anchor**, in that gizmo's frame — swivel
  about the surface normal, tilt about the tangents, contact point stays put.
  Rejected: rotating about the object's own centre (a tilt swings the contact
  point off the surface).
- **Ring drag is capped at ±180° per gesture and keeps no winding state.**
  Rejected: a winding counter permitting multi-turn spins, which a fast enough
  flick can miscount into a violent half-turn snap.

**Invariant worth naming, because the split makes it tempting to re-ask:** the
Shape gizmo's basis is `node.rotation` and nothing else — it never reads a
surface. Only Placement reads the tangent basis. A rotation set while a node is
free therefore survives any later attachment by construction; re-snapping would
change what Placement shows without touching Shape. No "inherit or preserve"
rule is needed, and none should be added.

Out of scope, deferred to the attachment spec that follows this one: a real
parent/child model, surface-constrained sliding (the node re-projecting onto the
parent's SDF as it is dragged), attach/detach as an operation, hierarchy UI, and
bone attachment. This design reads today's `snapped` / `snap_point` /
`snap_normal` fields as-is — a frozen record of where the node was born — and
does not make them live. Nothing here re-snaps: `snapped` is set only by
`spawn_snapped`, and `updateDrag` never re-evaluates it.

## 1. Rotation becomes real

`SdfNode` gains a third `sq_float4` holding the **inverse** rotation quaternion,
taking it from 32 to 48 bytes (`static_assert` updated). `sdf_scene.h` gains
`sdf_rotate(q, v)` beside the existing `sdf_min`/`sdf_length` portability
wrappers, using `v + 2·cross(q.xyz, cross(q.xyz, v) + q.w·v)` — Metal has no
quaternion type, so the formula is written once by hand rather than dispatched
per language. `sdf_eval_node` becomes `q = sdf_rotate(node.inv_rotation, p − pos)`.
`pack_scene` writes the conjugate of `node.rotation` (unit quaternion, so
conjugate == inverse).

Because the header is dual-compiled, this one edit covers the CPU evaluator
(`sdf.cpp`, and through it `evaluate_scene_sdf` / `sample_scene` / DCSDD) and the
GPU raymarch alike. Everything else already routes through matrices:
`raycast_scene` inverts `world_from_local`, and both `append_cube_edges` and
`append_sphere_outline` take the full matrix, so picking and wireframes are
correct for free.

**Scale must stay baked into the shape, never applied as a transform.** The usual
T-then-R-then-S warping hazard does not apply here and must not be introduced
while adding rotation: `pack_scene` writes `half_extents = scale * 0.5` and
`sdf_eval_node` hands that straight to `sd_box`, which is exact under nonuniform
half extents. The evaluation therefore translates, rotates, then measures the
shape at its true dimensions — there is no divide-into-unit-space step for a
non-uniform scale to distort. That is equivalent to the T·R·S of a unit primitive
that `Node::world_from_local()` builds and `raycast_scene` inverts, which is why
the two paths describe the same surface. `sdf_eval_node`'s comment records this,
because it is precisely the kind of thing a later reader "fixes" by adding a
`q / half_extents`.

The ellipsoid stays approximate under non-uniform radii — `sd_ellipsoid` is iq's
approximation and always was. Pre-existing and unaffected.

## 2. Gizmo frames

`gizmo_frame_for_node` takes a `GizmoSlot` instead of a `GizmoKind` and owns its
basis outright. The `Camera&` parameter stays, used only to size `half_extent`.

| Slot | node | origin | u / v / n |
|---|---|---|---|
| Placement | snapped | `snap_point` | tangent basis of `snap_normal` |
| Placement | free | `node.position` | node-local axes |
| Shape | either | `node.position` | node-local axes |

"Node-local axes" is `simd_act(node.rotation, X/Y/Z)` — identical to world axes at
identity rotation, so a scene that has never been rotated is unaffected.
`u × v == n` holds in every row: `tangent_basis` sets `v = n × u`, so
`u × v = u × (n × u) = n(u·u) − u(u·n) = n` identically.

`drag_plane_for_node` and `DragPlane` are **deleted**. `gizmo_frame_for_node` was
their only production consumer; once the free branch stops reading
`camera_forward`, what remains is a one-line branch on `snapped` that belongs
inside the frame builder rather than in `picking.h` behind a struct named for a
plane it no longer describes.

Note what does **not** change: Placement's origin is `snap_point` for a snapped
node and `node.position` for a free one, which is exactly what
`drag_plane_for_node` already returned. Move handles therefore keep the drag
plane they have always had, and this section is purely about the basis. What
makes the two gizmos stop jumping relative to each other is the spawn-placement
ruling bringing `node.position` onto `snap_point`, not a change of anchor.

## 3. The grid is a reference plane

`GizmoFrame` gains `grid_u, grid_v`: the plane the grid is drawn in, through
`origin`. Attached → `u, v`. Free → world X, Z. Shape sets them to `u, v` and
draws no grid, so there is no unset state to reason about.
`append_move_gizmo_grid` reads these instead of `u/v`; its signature and its
renderer call site are untouched.

This is a deliberate reclassification. `lines.h` previously asserted the grid was
a drag-**plane** affordance, and for a free node it no longer is — it shows XZ
while `PlaneUV` drags in XY. The comment changes, not the choice: a horizontal
reference through the object beats a vertical wall, and the three plane patches
already advertise their own planes.

## 4. Coalescing

Two anchors merge when

```
distance(placement.origin, shape.origin) < kGizmoCoalesceFrac * placement.half_extent
```

with `kGizmoCoalesceFrac = 0.35`. This is a world-space comparison and
deliberately not a projection: `half_extent` is already screen-constant, so the
test *is* a screen-space one, with no behind-camera or `w ≈ 0` edge cases to
guard.

## 5. Handles: bands, geometry, hit-testing

Every handle owns a disjoint radial band, as a fraction of `he`. The coalesced
gizmo's handle set is then the union of the two separated ones with nothing
overlapping — and a gizmo does not restructure itself as it merges or splits.

| Band | Handle | Slot |
|---|---|---|
| centre disc | uniform scale | Shape |
| inner shafts | move axes | Placement |
| mid patches | move planes | Placement |
| outer shafts + box tips | scale axes | Shape |
| outermost rings | rotate | Placement |

The band constants live in `gizmo.h` beside `kGizmoPatchInner/Outer`, as one
source of truth for both the draw and the pick — the "drawn = hit" invariant that
file's existing constants exist to protect.

`append_rotate_gizmo_rings` draws three camera-facing thick-quad rings, one per
axis, reusing the expansion `append_pivot_crosshair` already uses for its ring.

`pick_gizmos(placement, shape, coalesced, ray, fov, h)` returns
`GizmoHit { GizmoSlot slot; GizmoHandle handle; }`. Priority runs by band,
innermost first, then by screen-space distance — the same "smallest distance
wins, ties to smaller ray-t, then declaration order" rule `pick_axis_handle`
already implements, extended across two origins. Ring picking is `ray_plane`
against the ring's plane, then `|distance(hit, origin) − R|` against a points
tolerance converted at the hit's depth; `ray_plane`'s existing parallel guard is
what makes an edge-on ring unhittable, matching `kAxisViewAlignLimit`'s role for
axes.

**Band dimming.** Hovering any handle drops every handle in *other* bands to
`kGizmoHandleDimAlpha` while the hovered band keeps its rest alpha and the
hovered handle goes hot. A third alpha state, not new plumbing: both handle
builders already branch per handle between `kGizmoHandleRestAlpha` and
`kColorGizmoHot`. It targets the moment the density costs you — picking one
handle out of four concentric bands — without making the resting gizmo harder to
read.

**Anchor tether.** When the two gizmos are not coalesced, a thin low-alpha
segment runs from `snap_point` to `node.position`. It says the two clusters
belong to one node and shows how far the detail has been lifted off its surface —
the quantity the attachment spec will make live.

## 6. Rotation drag

Ring drag is stateless in the same sense every other gesture in this codebase is:
the result is a pure function of the current cursor position and the state frozen
at mouse-down. Both the press and each update intersect the ray with the ring's
plane and take the pivot-relative direction; the signed angle between them is
read about the ring's *known* axis.

```
v        = normalize(ray_plane(ray, o, axis) − o)      // v0 captured at press
theta    = atan2(dot(cross(v0, v), axis), dot(v0, v))  // signed, (−pi, pi]
q        = quaternion(theta, axis)
rotation = q * start_rotation
position = anchor + rotate(q, start_pos − anchor)
```

The last line is what keeps the contact point pinned; for a free node
`anchor == start_pos`, so it is a no-op and one formula covers both cases.
`snap_point` and `snap_normal` are untouched — the surface did not move. A
near-parallel ray keeps the last rotation, mirroring `ray_plane`'s guard in the
move path.

**Why a signed angle about a known axis, rather than a shortest-arc quaternion
between `v0` and `v`.** The two are equivalent while the vectors are distinct, but
the general `[dot, cross]` form degenerates at exactly antiparallel: `cross → 0`
and the axis becomes undefined. Here the ring's axis is known a priori and both
hits lie exactly in its plane by construction, so the signed-angle form is
well-conditioned right through ±π and the rotation axis can never drift off the
ring the user grabbed.

**Consequence, accepted:** a single drag cannot exceed half a turn — past ±180°
the shortest-path result reverses, and a multi-turn spin needs a second drag.
That is the price of keeping no winding state, and it buys the strict
cumulative-from-start invariant the camera and scale gestures already hold, with
no fast-flick failure mode.

## 7. Interop and app layer

`GizmoHandle` gains `RingU = 8, RingV = 9, RingN = 10`. New
`enum class GizmoSlot : int32_t { Placement = 0, Shape = 1 }` and
`struct GizmoHit { GizmoSlot slot; GizmoHandle handle; }`; `gizmoHoverHandle()`
returns the latter. `Renderer::set_gizmo` becomes
`set_gizmos(placement, shape, coalesced, hover, eye)`.

**`GizmoKind` and `setGizmoKind` are removed.** With both gizmos live there is no
kind to select, so the interop surface shrinks rather than grows.

Swift loses `RadialTool`, `activeRadialTool`, `radialSelectMove`,
`radialSelectScale` and the mouse-up scale-revert branch; `syncGizmo` keeps only
`setGizmoVisible`. `RadialMenu` drops its Move and Scale buttons, keeping the op
toggle and delete. The pointer state machine, the camera gestures and the drag
threshold are untouched — this design adds no input plumbing at all.

## 8. Testing

- **`sdf_tests.cpp`** — a box rotated 45° about Y against the closed form at
  points a rotation-blind implementation gets wrong (mutation-checked: the case
  must fail with the rotation term removed); an ellipsoid rotated about an oblique
  axis; identity rotation reproducing every existing expectation bit-for-bit.
- **`picking_tests.cpp`** — `raycast_scene` on a rotated node (existing behaviour,
  previously unreachable). **Cross-validation:** raycast a rotated *and*
  non-uniformly scaled node and assert the hit point sits on the SDF's zero-set.
  This is the assertion that fails if the two paths ever stop describing the same
  surface, and therefore the one that catches a scale-as-transform mistake.
- **`gizmo_tests.cpp`** — the §2 frame table; the property that is the original
  bug (build both frames from two very different cameras, assert `origin`, `u`,
  `v`, `n`, `grid_u`, `grid_v` unchanged, snapped and free alike); coalescing
  either side of the threshold; local axes tracking a non-identity rotation; the
  band priority chain, coalesced and separated; a ring hit at its radius and a
  miss just off it. The two deleted `drag_plane_for_node` cases move here as frame
  cases.
- **`lines_tests.cpp`** — ring vertex counts and drawn == hit radii; a free node's
  grid lying in world XZ.
- **Rotation drag** — a quarter turn on each ring recovers the expected
  quaternion; a snapped node's `snap_point` stays exactly fixed while `position`
  swings; a free node rotates in place; dragging back to the press angle restores
  the start rotation exactly; ten updates ending at one cursor position match a
  single update there (idempotence, as the camera gestures pin); a near-parallel
  ray leaves the rotation untouched.
- **`scene_tests.cpp`** — the spawn-offset cases update to the new placement.

App layer has no Swift tests; verified by build plus hands-on, as the previous
specs did.

## 9. Implementation hazards

1. **Handle density on a coalesced gizmo.** Four concentric bands plus three rings
   around one origin is the "universal manipulator" pattern, and it is genuinely
   busy — the sharpest risk here, because seeing the surface curve matters more
   than seeing the manipulator. Disjoint bands, the hover highlight and band
   dimming are the mitigations. If it still reads as clutter, drop the move plane
   patches first: least-used handles, widest area. This needs eyes on it, not a
   test.
2. **`SdfNode` 32 → 48 bytes** grows the per-frame node upload by half. Irrelevant
   at current node counts, and the right trade for a raymarcher: packing the
   quaternion into three floats and reconstructing `w` would move cost from a
   once-per-frame upload into the inner sphere-trace loop, where it is paid per
   step per pixel.
3. **Feel constants.** The ring pick tolerance and `kGizmoHandleDimAlpha` are
   tunables in the same family as `kScaleAxisPickTolerancePts` (14pt), which the
   camera work already had to set by hand. Expect the same here.
4. **The spawn placement change is visible in existing scenes' semantics**, not
   just new ones: a detail now sits half inside its parent rather than resting on
   it. That is the intent, but it changes what an `Add` node looks like at spawn
   and is worth confirming by eye before the rest of the branch is judged.
