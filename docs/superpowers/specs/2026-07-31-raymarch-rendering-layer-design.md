# Per-pixel sphere-tracing rendering layer + DCSDD dormancy — design

Approved 2026-07-31. Stage 3: the primary viewport view becomes a per-pixel
sphere-traced render of the analytic CSG SDF — evaluated every frame, so edits and
drags are visible live. DCSDD (stage 2) goes dormant: unplugged from the editor,
modules and tests kept for its later return.

User rulings: raymarching replaces the DCSDD mesh as primary view; DCSDD dormant
("no draw, no auto-remesh"); shading stays normal-colored debug for now.

Also resolves three open code-review findings by construction (see §6).

## 1. Single source of SDF truth — `shaders/sdf_scene.h` (dual-compile)

New header compiling as both MSL and C++ (same idiom as `shared_types.h`):

- `SdfNode { float4 pos_shape; float4 half_extents_op; }` — 32 B, static_asserted.
  `pos_shape.xyz` = world position, `.w` = shape (0 = cube, 1 = sphere);
  `half_extents_op.xyz` = `scale·0.5`, `.w` = op (0 = add, 1 = subtract).
- The SDF evaluation functions (exact box, iq ellipsoid with origin guard, CSG fold in
  node order: add → min, subtract → max(d, −d_node)) move here from `sdf.cpp`, which
  is refactored to consume them — the existing numpy-pinned `sdf_tests` then pin the
  exact code the GPU runs. Parity by construction.
- The per-pixel ray-generation formula also lives here (pixel + viewport + inverse
  view-projection → world ray): fragment framebuffer coords are top-left-origin in
  Metal, matching the project's view-point convention, and the formula must agree
  exactly with `Camera::ray_through_view_point` (tested).
- `pack_scene(SceneDocument) → std::vector<SdfNode>` (in `sdf.{h,cpp}`), capped at
  `kMaxRaymarchNodes = 128` (setFragmentBytes' 4 KB budget; assert in debug, clamp in
  release with a comment).

## 2. Raymarch pass — `shaders/raymarch.metal`

- Fullscreen triangle from `vertex_id`, no vertex buffer.
- `RaymarchUniforms { float4x4 view_proj; float4x4 inv_view_proj; float4 eye;
  float4 params }` (node count, near, far in params; static_asserted, dual-compile,
  lives in `shared_types.h` with the other GPU-shared render types).
- Fragment: generate ray via the shared formula; sphere-trace — start at near (0.1),
  max 128 steps, hit epsilon scaled with distance (`max(1e-4, 5e-4·t)`), tmax = far
  (100). Miss → `discard_fragment()` (clear color shows through).
- Hit: normal via tetrahedron-offset gradient of the same SDF; color `0.5·(n+1)`
  (consistent with the debug look). Fragment writes real depth: project the hit point
  with `view_proj`, output `clip.z/clip.w` via `[[depth(any)]]` — the depth buffer
  stays meaningful for any future depth-tested pass.

## 3. Renderer & editor wiring

- Renderer: one new PSO (raymarch — RGBA16Float + Depth32Float, fragment depth
  output, no blending) + `set_raymarch_scene(nodes, uniforms)`-style per-frame input
  via `setFragmentBytes` (≤ 4 KB, no buffers, no dirty tracking). Draw order:
  raymarch → scene lines → gizmo (lines depth-ignored on top, as today). The stage-2
  mesh path stays compiled but is never fed.
- Editor `render()`: packs the node array + uniforms every frame — live-during-drag
  falls out for free.

## 4. DCSDD dormancy

Editor loses: the `MeshJobRunner` member, `latest_mesh`, every `requestRemesh()` call
site, the drag/scale `moved` flags (now dead), and `editor_mesh_config()`. The
`dcsdd`, `sdf`, and `mesh_job` modules and ALL their tests stay compiled and green.
Re-enabling later is wiring, not archaeology.

## 5. Wireframe policy

`build_scene_lines` drops the `mesh_present` parameter: wireframes draw for the
selected node only, always — the raymarched view can never be stale, so the policy
needs no presence proxy. Lines tests updated accordingly.

## 6. Code-review findings resolved by this stage

- renderer.cpp:223 stale-mesh wireframe window — the `mesh_present` proxy is deleted
  (§5), not patched.
- editor.cpp:82 CoreTests remesh storm / exit-time worker race — no `requestRemesh()`
  call sites remain (§4); verified by the suite's wall-clock returning to pre-stage-2
  levels (reported, not asserted — exit races aren't assertable in doctest).
- editor.cpp:224 stale `drag.moved` — the flags are deleted as dead code (§4).

## 7. Testing

- CPU red/green: `pack_scene` pinned (shapes/ops/scales/cap behavior); shared ray-gen
  formula equals `Camera::ray_through_view_point` at the existing pinned pixels;
  `sdf_tests` stay green across the shared-header refactor (the guard); lines-policy
  tests updated to selected-only-always.
- GPU: `MTL_DEBUG_LAYER=1` runtime smoke launch (D7 pattern) — no validation errors.
  Visual verification is the owner's hands-on pass.

## 8. Out of scope

Lighting, materials, AO, shadows, anti-aliasing, half-res/adaptive stepping, UI
changes, DCSDD re-enablement, rotations.
