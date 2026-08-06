# Eight shapes, and a dial for what the box cannot say — design

**Corrected during implementation (2026-08-06).** §1 claimed iq's
`sdVesicaSegment` "handles `r > hy` … without special-casing". It does not, and
the shape now uses a rewritten region test — see §1's vesica entry and the
function's own comment. Found by checking every formula against an independent
numpy transcription *before* writing the C++, which is also how §9's first
hazard was caught: the prism's radius convention is indeed easy to read
backwards, though it turned out the code had it right and the first probe
points were the thing that was wrong.

Approved 2026-08-06. Takes the primitive set from two (cube, sphere) to eight,
adding **cone, capsule, octahedron, pyramid, prism and vesica segment**, and
gives four of them a single extra parameter driven by an arc dial on the radial
menu's lower semicircle.

Motivation: the app targets **stylized character/creature models and procedural
face generation**. Two primitives cover volume and mass and nothing else — a
limb, a horn, a fin, an eyelid and a faceted gem all have to be faked out of
stretched ellipsoids today. The six additions are chosen for that domain rather
than for completeness of iq's catalogue.

The design question this raised, which the codebase had never had to answer:
a node's transform supplies three numbers (`scale`, halved into half-extents),
and those pin every shape's **size**. What they cannot state is a shape's
*profile* — how blunt a cone's tip is, how round a capsule's caps are, how many
faces a prism has. That leftover degree of freedom is what the dial is for, and
the rule is the same for every shape rather than a special case per shape.

Two subsystems have no general path today and are **replaced** rather than
extended:

- **Picking** intersects hand-written analytic primitives (`ray_unit_cube`,
  `ray_unit_sphere`). Six more means deriving ray tests for a vesica of
  revolution, an n-gon prism whose side count is a runtime parameter, and two
  frusta — and every future shape pays the toll again.
- **`SdfNode` is 48 bytes with every float spoken for.** `pos_shape.w` is the
  shape, `half_extents_op.w` is the op; there is no spare slot for a parameter.

## User rulings

Settled by a question-at-a-time design pass:

- **A dial only where the box leaves a degree of freedom genuinely free.** Cone,
  pyramid, capsule and prism get one. Cube, sphere, octahedron and vesica do
  not. The vesica is the interesting rejection: its pointiness *is* its box
  aspect — length from `scale.y`, width from `scale.x` — so a dial would
  duplicate the Shape gizmo and give two controls one meaning. Rejected
  alternatives: a dial on every shape (invents a DOF for the two that have
  none), and no dials at all in v1 (does the packing and UI work twice).
- **Continuous dials snap to 0.05; the prism's side count snaps to whole
  numbers.** 21 detents across the sweep for the former, 10 for the latter.
- **The dial is an arc on the radial menu's lower semicircle**, at the same
  64 pt radius the two action buttons use. **Upper half acts** (op, delete),
  **lower half sets**. The knob's resting angle *is* the value, which is what
  makes absolute mapping and "no jump on grab" the same thing rather than
  opposites — you always grab the knob where it already is, and one 180° sweep
  still reaches both ends. Rejected: an arc on the upper semicircle (fights the
  two action buttons for the same radius, and pressing a fixed button at 90°
  with the knob elsewhere is exactly the jump-on-grab `gizmo.h` rules out for
  the scale handles); an unfurling labelled track (costs the most screen
  directly above the model); and rate-based scrubbing with no track (range
  invisible until you hit an end).
- **Picking: KISS.** One local sphere trace for every shape, optimised later if
  it ever shows up in a profile. Rejected: hand-writing six more analytic
  intersectors, and tracing the whole CSG scene (attractive — it would retire
  the carved-region limitation — but it needs a separate path for Subtract
  nodes, which have no surface in the fold, and that is a scope expansion
  beyond adding shapes).

**Deliberately not in scope:** materials, smooth/blended CSG unions, per-shape
UV or texture parameters, and more than one parameter per shape. `params.yzw`
is reserved so the third of those does not require another packing change.

## 1. The shape library

Local frame, **Y is the distinguished axis** for every new shape.
`h = scale · 0.5`, and `r = min(hx, hz)`.

| Shape | id | Local definition | Dial | Default |
|---|---|---|---|---|
| Cube | 0 | exact box — **unchanged** | — | — |
| Sphere | 1 | ellipsoid approximation — **unchanged** | — | — |
| Cone | 2 | capped cone, base radius `r` at −hy, top radius `k·r` at +hy | tip `k` 0…1 | 0 |
| Capsule | 3 | rounded cylinder: radius `r`, half-height `hy`, edge round `rb = c·min(r, hy)` | round `c` 0…1 | 1 |
| Octahedron | 4 | `‖q‖₁ = s`, `s = min(hx, hy, hz)` | — | — |
| Pyramid | 5 | square frustum, base half-size `r`, top half-size `k·r`, half-height `hy` | tip `k` 0…1 | 0 |
| Prism | 6 | regular N-gon inscribed in the xz box, extruded to ±hy | sides `N` 3…12 | 6 |
| Vesica | 7 | vesica of revolution, half-length `hy`, mid half-width `r` | — | — |

Ids 0 and 1 must not move: the shader compares `pos_shape.w` against them.

**Capsule as a rounded cylinder** is what makes its dial honest. `c = 0` is a
flat-capped cylinder, `c = 1` a true capsule, and the awkward case — a box
wider than it is tall — comes out as a disc with a fully rounded rim rather
than as a degenerate shape needing a clamp. Writing it as a plain capsule
instead would have left `hy < r` undefined and the dial with nothing to say.
Cone at `k = 1` is also a cylinder; the redundancy costs nothing and both
readings are reachable from the shape the user actually picked.

Formulas are iq's (<https://iquilezles.org/articles/distfunctions/>),
specialised to the Y axis so the general endpoint math drops out:

- **Cone** → `sdCappedCone(p, h = hy, r1 = r, r2 = k·r)`. Exact.
- **Capsule** → `d = (length(p.xz) − (r − rb), |p.y| − (hy − rb))`, then
  `min(max(d.x, d.y), 0) + length(max(d, 0)) − rb`. Exact.
- **Octahedron** → `sdOctahedron(p, s)`, the exact branch rather than the
  cheap bound.
- **Prism** → iq's exact regular-polygon 2D distance (sector fold via `atan`),
  then convex extrusion against `|p.y| − hy`. Exact.
- **Vesica** → the profile arc through `(0, ±hy)` and `(r, 0)`, revolved.
  `r == hy` degenerates to a sphere and `r > hy` to a bi-cusped spinning-top,
  both valid shapes that a user can reach by dragging the box — so neither may
  be clamped away. **This is NOT iq's `sdVesicaSegment` inlined.** His region
  test is a single angular comparison, valid only while the profile arc is the
  minor one; once `r > hy` the arc becomes major, the test wraps, and points
  deep inside the solid near the axis come back with a *positive* distance.
  The replacement projects onto the full circle and asks whether that
  projection lands on the arc, which is orientation-free and so holds for the
  minor arc, the major arc and the semicircle alike — and reproduces his answer
  exactly wherever his is valid.
- **Pyramid frustum** → **max of six half-space distances** with unit normals:
  four slant planes plus top and bottom. This is the one approximation being
  added — exact on the surface and throughout the interior, conservative
  outside near edges and corners, and 1-Lipschitz everywhere. The same bargain
  `sdf_sd_ellipsoid` already makes, and it is why `k = 1` yields a field that
  is correct but weaker than `sdf_sd_box`'s on the same shape.

## 2. Non-uniform scale: contract, never dilate

`sdf_eval_node` carries an emphatic comment forbidding division of `q` by
half-extents, on the grounds that distances come back in a scaled space and the
sphere trace oversteps. That comment is about **dilation**, and it is right.
Every new shape uses a **contraction** instead:

```
r = min(hx, hz)
q ↦ (q.x · r/hx,  q.y,  q.z · r/hz)      // both factors ≤ 1
```

followed by the exact primitive at **real dimensions** `(r, hy)` — not
unit-normalised ones. The octahedron, which distinguishes no axis, uses the
three-axis version with `s = min(hx, hy, hz)`.

Why this is safe without a correction factor: for `T = diag(a, 1, b)` with
`a, b ≤ 1`, `|T p − T q| ≤ |p − q|`, so `g(T p)` inherits `g`'s 1-Lipschitz
property in world space, and its zero set is exactly `T⁻¹(∂S)` — the shape we
want. A 1-Lipschitz function vanishing exactly on the surface never exceeds the
true distance, which is precisely the sphere trace's requirement. **When
`hx == hz` the map is the identity and the distance is exact**, which covers a
capsule stretched 6:1 along Y — i.e. most of character work.

The crude alternative, `sd_unit(q / h) · min(h)`, has the same zero set but
underestimates distances by up to `max(h)/min(h)` *everywhere*, including the
axially-stretched case. At a 6:1 aspect that is a sixth of the step size
against a 128-step budget, and it starves the trace exactly where this domain
lives. It must not be used.

`scene_aabb` needs no change — every shape is inscribed in its box. (It also
ignores rotation, which is a pre-existing looseness and out of scope here.)

## 3. Packing

`SdfNode` gains a fourth `sq_float4 params`, taking it from 48 to 64 bytes
(`static_assert` updated). `params.x` is the dial value; `yzw` are reserved so
a second parameter never forces another packing change. `renderer.cpp` already
sizes the node buffer from `sizeof(SdfNode)`, so the upload path is untouched.

`sdf_eval_node` becomes a `switch (int(node.pos_shape.w))` passing
`node.params.x`; the cube and sphere branches keep their exact evaluators
rather than being folded into the contraction scheme. Every shape body is
written in the dual-compile idiom (`sdf_min`, `sdf_length`, …) so one
definition serves `raymarch.metal` and the CPU sampler both — new wrappers
(`sdf_clamp`, `sdf_atan2`, `sdf_sign`, `sdf_dot`) are added to the toolkit as
the shapes need them.

## 4. The parameter, and its single source of truth

`Node` gains `float shape_param`. Its range, step, default and formatting live
in **one table**, surfaced as:

```c++
struct ShapeParamSpec {
    bool  has_param;
    float min_value, max_value, step, default_value;
    bool  integral;   // formatting: "6", not "6.00"
};
```

`Editor::shapeParamSpec(Shape)` exposes it, and Swift reads the same numbers
for the arc's ends, its detents and its readout. Core clamps and snaps inside
`setNodeShapeParam`, so a value is valid regardless of who wrote it; Swift
snaps only for display. This is the `kGizmoPatchInner/Outer` precedent — the
constants that drifted between draw and pick until they were shared — applied
before the drift can happen.

`Editor` also gains `nodeShape(id)`, which Swift needs to choose a spec, and
`nodeShapeParam` / `setNodeShapeParam`. `shape_name()` and the two per-shape
name counters become tables indexed by shape; `make_node` seeds `shape_param`
from `default_value`.

## 5. Picking becomes a sphere trace

`raycast_scene` marches each node's local SDF and both analytic intersectors
are deleted, along with their direct tests.

The transform changes as well as the test. Today's code inverts
`world_from_local` into the **unit-primitive frame** — the warped space where
scale has been divided out — which is why it has to recover world `t` from the
hit point afterwards. The trace uses the **rigid frame** instead,
`q = rotate(inv_rotation, p − position)`, the same frame `sdf_eval_node` works
in. That frame preserves distance, so:

- `t` comes back in world units directly, and the world-`t` recovery goes away;
- the local direction stays unit without renormalising;
- the per-node `simd_inverse` disappears.

Two details carry existing behaviour forward:

- **March by `|d|`, not `d`.** From inside a node, `|d|` is still a lower bound
  on the distance to the boundary, so the same loop converges on the *exit*
  surface — which is the behaviour `picking_tests` already pins ("an origin
  inside a node returns its exit face"), and it matters because dollying in
  puts the eye inside geometry. A small step floor keeps the loop from stalling
  on a zero crossing.
- **Normal from the tetrahedron-offset gradient**, mirroring `raymarch.metal`'s
  four-tap pattern. It points outward on entry and exit hits alike, which is
  the convention the analytic tests assert.

CSG-blindness is unchanged: a region carved away by a Subtract node still
reports a hit, exactly as the README's limitations list says today.

## 6. Wireframes, framing, spawning

`build_scene_lines` dispatches on shape and passes `shape_param`, following the
split already there: **real edges** for the shapes that have them (cone → base
ring plus slant lines; pyramid → its 8 or 12 edges; prism → two N-gon rings
plus N verticals; octahedron → 12 edges), and a **camera-facing silhouette**
for the round ones (capsule, vesica), as `append_sphere_outline` does for the
sphere.

`node_bounding_radius` uses `simd_length(half)` for the six new shapes — a
correct bound for all of them. The cube and sphere branches are untouched so
framing behaviour does not shift for existing scenes.

`SpawnOptionsBar` grows to eight icons in the existing segmented row (~252 pt,
which still fits under the mode bar).

## 7. The arc dial

The two action buttons stay exactly where they are, at 135° and 45° on radius
64. When the selected shape's spec has a parameter, a knob is drawn on the
**lower** semicircle at the same radius, at

```
angle = 180° + 180° · (value − min) / (max − min)
```

so its resting position reads the value without a number. A `DragGesture` in
the menu's local coordinate space gives the cursor's offset from the frame
centre — which *is* the anchor — so the value is an `atan2` of that offset, with
no round trip through the viewport and no coordinate-space reconciliation.
Track, detents and a numeric readout draw **only while held**, the way the
placement grid draws only during a Placement drag.

The existing `.frame(extent · 2)` already contains a 64 pt radius knob, and the
lower half stays click-through apart from the knob's own circle — so the
"no full-screen `contentShape`" property `ContentView` documents survives.

**The dial drag must not set `isDragging`.** `ContentView` hides the entire
radial menu while that flag is up — it exists so the menu does not fight a
gizmo drag — so reusing it here would make the menu vanish underneath the
gesture driving it.

## 8. Testing

Per shape, in `sdf_tests.cpp`:

- **zero set** at hand-picked surface points, and **bbox tightness** — each
  shape touches the box faces it should, which is the assertion that catches an
  apothem/circumradius mix-up in the prism or a mis-scaled contraction;
- **a 1-Lipschitz spot check** over random point pairs. This is the property
  §2 rests on, and the one thing that breaks silently if a contraction is ever
  written as a dilation;
- **parameter endpoints collapsing onto shapes already trusted**: cone `k = 1`
  against a cylinder cross-check, capsule `c = 1` at `hy = hx` against
  `sdf_sd_ellipsoid`, pyramid `k = 1` against `sdf_sd_box`, prism at `N = 12`
  approaching a cylinder.

In `picking_tests.cpp`, the assertions pinning analytic literals get explicit
tolerances — doctest's default relative epsilon is far tighter than a trace
converges. The test worth keeping is the one already there: **`raycast_scene`
hits evaluate to ~0 under `evaluate_scene_sdf`**, extended from four cases to
all eight shapes with and without rotation and non-uniform scale. It is the
cross-check between the two independent descriptions of a surface, and after
this change it is also the only one, since the analytic path it used to
cross-check against is gone.

Clamp and snap get their own cases: `0.47 → 0.45`, `6.4 → 6`, out-of-range
values clamping to the spec's ends.

## 9. Implementation hazards

- **The prism's radius convention.** iq's regular-polygon SDF is parameterised
  in a way that is easy to read backwards; an apothem/circumradius mix-up
  produces a shape that looks plausible but does not fill its box. The bbox
  test is the guard.
- **Picking accuracy is now epsilon-bounded**, where it used to be exact. A
  snapped spawn puts the new node's *centre* on the hit point, so the error is
  invisible in use — but it is a real behaviour change, and it is why the
  literal-pinned assertions have to move to tolerances rather than being
  tightened.
- **A grazing ray on an anisotropic cross-section** (`hx ≠ hz`) is the one
  place §2's contraction is conservative, and the one place to watch for trace
  break-up at silhouettes.
- **The pyramid frustum's field is weaker than the box's** on the same shape at
  `k = 1`. Special-casing to `sdf_sd_box` there is available if it ever
  matters; it is not worth the branch up front.
