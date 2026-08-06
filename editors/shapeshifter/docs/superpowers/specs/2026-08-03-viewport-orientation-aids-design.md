# Viewport orientation aids — design

Approved 2026-08-03. Adds the world-space orientation layer the viewport has
never had — a ground plate on y=0 with origin axis markers — and reworks the
two pieces of editor chrome that were standing in for it badly: the
move-gizmo's grid and the always-on camera-pivot marker.

Motivation: the app exists to sculpt objects **around the origin** (character
models, buildings). That makes three things load-bearing — a ground reference
to sit things on, an unambiguous origin with readable axis direction, and
manipulator chrome that stays subordinate to the model. Before this work the
viewport had none of them: the only grid on screen belonged to the move gizmo
(node tangent frame, screen-constant, swimming with the camera for unsnapped
nodes), and the pivot marker was a 18-segment "spiked cube" drawn every frame
in every mode.

Note on axes: **Y is up** (`CameraController` hardcodes `+Y`, `camera.up =
{0,1,0}`), so the horizontal ground plane is **XZ**, i.e. y=0. The original
request said "the xy plane"; corrected here.

## User rulings

Settled by hands-on review of live perspective mockups (preserved in
`.superpowers/brainstorm/`), each judged on the real background colour with a
block at the origin for occlusion and scale:

- **Grid extent: finite plate, half-extent 30** (60×60). Rejected: infinite
  adaptive-fade (Blender/Unity style) and infinite-plus-build-volume. The
  plate is a literal ruler — fixed extent, fixed spacing, no LOD.
- **Grid tiering: tiered** — faint line every 1 unit, brighter every 5,
  brightest at the border, so distance is countable in fives. Rejected: plain
  single-weight, and zoom-aware tier fading.
- **Axis colours: desaturated** RGB, not full-saturation primaries — at world
  scale full RGB reads as a toy and fights the gizmo for attention.
- **Grid is depth-tested**: objects resting on the floor occlude the grid
  behind them. This is the cue that answers "is this on the ground or
  floating?", and it is a real change — every other overlay in the renderer
  draws depth-ignored.
- **Pivot marker: transient.** Nothing at rest; a flat screen-space ring +
  crosshair during camera gestures, fading out after. The pivot is feedback
  about a gesture in progress; persistent 3D geometry at the orbit target is
  clutter that also isn't part of the model. Rejected: always-on minimal
  marker, and removing it entirely.
- **Gizmo grid: kept, restyled** — the soft-edged square (radial fade to
  nothing, no hard edge). Rejected: dropping it, polar rings, dot field, and
  the tinted positive quadrant.
- **Gizmo handles: "refined RGB"** — the palette stays, everything else
  calms down. Explicit user note: the handles were "too cartoonish".
  Rejected: tapered cones, hairline+ring, and a neutral palette with only the
  normal accented.
- **Gizmo size: unchanged.** `kGizmoScreenFraction` stays 0.24 — the R2
  ruling stands; the restyle alone does the de-cartooning.

Out of scope, sequenced next: the corner axis gizmo / ViewCube. Also out of
scope: a grid on/off toggle (YAGNI), any camera behaviour change.

## 1. Ground plate — a shader pass, not line geometry

A ±30 plate with 1-unit spacing drawn as `MTL::PrimitiveTypeLine` geometry
would moiré into a smear at full zoom-out (61 minor lines per direction). A
fragment shader with derivative-based coverage fades over-dense lines toward
the background instead — which is exactly what makes the fixed-spacing
finite-plate ruling safe **without** any zoom-aware logic, and it delivers
correct depth occlusion for free.

### `shaders/ground_grid.h` (dual-compile)

Follows the `shaders/sdf_scene.h` precedent: compiled under both
`__METAL_VERSION__` and plain C++, so the math is unit-testable from
`tests/core` with no Metal device involved. All the real logic lives here as
one function:

```
ground_grid_shade(float2 q, float2 dq, GroundGridStyle) -> float4
```

`q` is the ground hit point's `.xz`; `dq` is `fwidth(q)` in the shader and a
hand-supplied input in tests. Coverage is computed in grid space —
`e = |fract(q/s − 0.5) − 0.5| / fwidth(q/s)`, `cov = 1 − min(min(e.x,e.y),
1)`. When lines pack tighter than a pixel `dq` grows and coverage decays to
zero on its own; that decay is the property the ±30 ruling depends on, and it
is pinned by test.

Tiers, composited with minor suppressed under major: minor 1u α0.15, major 5u
α0.36, plate border α0.45 (white); axes α0.60 on the positive half, α0.24 on
the negative — direction readable without labels. Zero alpha outside the
extent.

This header is also the **single source of truth for the axis palette**
(`kGroundAxisX/Y/Z`). The shader reads X/Z from it and `lines.cpp` reads Y
from the same constants, so the palette is never duplicated across the
language boundary.

### `shaders/ground_grid.metal`

Plumbing only — fullscreen triangle (same `kFullscreenTrianglePositions`
trick as `raymarch_vertex`), and a fragment that reuses
`sdf_ray_for_pixel` (`sdf_scene.h`) to reconstruct the world ray from
`inv_view_proj` alone, intersects y=0, calls the shared function, and writes
real depth via `[[depth(any)]]` from `view_proj * float4(p,1)`.

Two ordering rules in that fragment are load-bearing (see §6).

### Renderer

`GroundGridUniforms` in `shared_types.h` (160 B, same shape as
`RaymarchUniforms`), built by a pure `build_ground_grid_uniforms` mirroring
`build_raymarch_uniforms`. New alpha-blended `ground_pso_`, drawn with the
**existing** `depth_read_less_` state (Less, write off). Draw order becomes:
raymarch → mesh → **ground plate** → scene lines → gizmo → pivot.

## 2. Origin markers

X and Z live in the shader (they are on the plane). The +Y axis and the
origin pip are above it, so they are geometry: `append_origin_marker` in
`lines.cpp` emits a 3-unit +Y thick segment plus a small camera-facing white
pip, reusing the file-local `append_thick_segment`. Drawn in the same blended
`depth_read_less_` pass as the plate so plate and axes occlude consistently.

## 3. Gizmo restyle

**Grid.** Same 12 divisions and square footprint, with a radial alpha fade
(peak 0.20, → 0 between 0.45·he and 0.98·he) baked into `LineVertex.color.a`.
A 2-vertex line would interpolate that fade linearly and look wrong, so each
grid line subdivides into 12 segments.

**Handles.** Axis colours switch to the desaturated `kGroundAxisX/Y/Z`;
`kGizmoHandleHalfWidthFrac` 0.02 → 0.011; shafts end at 0.94·he with a
camera-facing terminator dot; plane patches become **filled** quads (fill
α0.16 + hairline border α0.45) pulled inboard to `[0.24, 0.50]·he`; resting
alpha 0.62 with `kColorGizmoHot` (white) on hover — the hover mechanism is
unchanged, it simply no longer competes with everything at full brightness.

**Drawn = hit stays load-bearing.** The patch bounds move in lockstep with
`pick_gizmo_handle` via shared `kGizmoPatchInner`/`kGizmoPatchOuter` in
`gizmo.h`, replacing the three independent sets of `0.3f`/`0.6f` literals
that previously lived in `lines.cpp`, `gizmo.cpp` and the tests.

**Vertex budget.** The restyle exceeds Metal's 4 KB `setVertexBytes` ceiling
(grid ≈624 verts; handles ≈132 against a 128 limit), so gizmo and pivot
geometry move to per-frame `MTL::Buffer`s through a small upload helper —
same replace-on-update pattern `raymarch_nodes_` documents (an in-flight
command buffer retains the old buffer via ARC). This retires the budget
arithmetic previously threaded through `lines.h`'s comments.

Pooling those uploads was raised in review and **deliberately deferred**:
ring-indexing by frame does not on its own guarantee the CPU writes only to
buffers the GPU has finished reading (that needs a `dispatch_semaphore` from
`addCompletedHandler`, and this codebase has no CPU/GPU sync primitives at
all), so a ring without the semaphore would trade a race-free pattern for a
racy one. Volume is ~30 KB/frame and bounded, where `raymarch_nodes_` already
allocates per frame at unbounded size without issue. Revisit only on a
profile showing `newBuffer` material in a capture, and then for all four
transient uploads together plus the semaphore.

## 4. Transient pivot crosshair

Removed: `append_spiked_cube`, `kColorPivotFront`/`Behind`,
`kPivotScreenFraction`, `kPivotLineHalfWidthPts`, the `depth_read_greater_`
state (nothing else used it) and the two-pass front/behind draw.

Added: `append_pivot_crosshair` — a flat camera-facing ring (screen-constant
14 pt radius, 24 segments) with four ticks crossing it, white, drawn last and
**depth-ignored** in a single pass. It is gesture feedback and should never
be occluded.

**Timing.** `Editor::render` has no time source, and scroll/magnify carry no
gesture end — so rather than plumbing gesture begin/end through the view
model, the marker keys off a **last-camera-activity timestamp**:
`cameraOrbit`/`cameraZoom`/`cameraPan` stamp a `std::chrono::steady_clock`
point inside their existing "did the controller actually move?" checks, so a
no-op gesture never lights the marker. `pivot_marker_alpha(seconds)` in
`gizmo.h` is pure and unit-tested: 1.0 while held (0.25 s), smoothstep → 0
over the next 0.35 s. `render()` skips the draw entirely at α ≤ 0.

This is uniform across drag-pan, scroll and pinch, and needs **zero interop
changes** — `ShapeshifterCore.h` is untouched by this entire feature.

Because the fade is keyed to elapsed time rather than a frame counter, a
reduced or variable refresh rate changes sampling density, not duration. The
display link already runs every vsync while in-window, so the fade adds no
power cost.

## 5. Testing

New `tests/core/ground_grid_tests.cpp` against the dual-compile header:
on-line vs between-line coverage, minor suppressed under major, axis sign
split, zero alpha outside the extent, border term, and coverage decaying
toward 0 as `dq` grows. New `pivot_marker_alpha` cases (value at 0 and at the
hold boundary, 0 past total, monotonic non-increasing).
`renderer_tests.cpp` gains the uniform-builder mapping.

Existing tests that change by construction: `gizmo_tests.cpp` patch bounds
move to the shared constants (plus a case asserting draw and pick read the
same ones); `lines_tests.cpp` layout pins are rewritten and the
`append_spiked_cube` case deleted; `drag_tests.cpp`'s patch grab point,
previously the literal `0.45f * he * (f.u + f.v)` — the centre of the old
`[0.3, 0.6]` patch — is re-expressed from the shared constants so it cannot
drift again.

App layer has no Swift tests (house convention); verified by running.

## 6. Implementation hazards

1. **Derivatives across discarded fragments.** `fwidth` is a 2×2-quad
   operation. If a lane exits before computing `q`, its neighbours get
   garbage derivatives — sparkle along the plate edge and at the horizon.
   Compute `q` and `fwidth(q)` unconditionally, before every rejection, and
   use `discard_fragment()` rather than an early `return`.
2. **Horizon NaN/∞.** Rays near-parallel to y=0 give enormous `t`;
   exactly-parallel rays give ±inf, and NaN when the camera sits at y=0. This
   is not a rare camera pose — rays are per-pixel, so whenever the horizon is
   on screen (any low-pitch camera; `kPitchLimit` prevents the pole flip, not
   a near-horizontal view) a row of fragments has `dir.y` crossing zero every
   frame. A `t ≤ 0` test does **not** catch NaN (IEEE comparisons against NaN
   are false), so a poisoned `q` would corrupt its whole 2×2 quad's
   derivatives. Mitigation: intersect with a sign-preserving clamped
   denominator so `t` is finite by construction, carry a reject flag, and
   discard only after `q`/`fwidth(q)` are computed. A small grazing fade on
   `|dir.y|` is cheap insurance on top.
3. **Depth range.** A plate corner can exceed the 100 far plane at the
   camera's 90 radius clamp, so `clip.z/clip.w` can land outside [0,1].
   Discard those fragments explicitly rather than relying on clamp behaviour.
4. **Empty scene.** The raymarch pass is skipped at zero nodes, leaving depth
   cleared to 1.0 — the plate's Less test still passes. Correct as-is.
5. **Camera below the plate.** The grid renders from underneath too;
   accepted.
