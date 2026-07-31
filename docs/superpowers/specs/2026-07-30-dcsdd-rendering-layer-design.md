# DCSDD rendering layer + delete op — design

Approved 2026-07-30. Two features: a delete op in modify mode, and the first real
rendering layer — solid mesh reconstruction of the CSG scene via **Dual Contouring of
Signed Distance Data** (Carrera, Wang, Batty, Stein, Sellán — SIGGRAPH 2026;
`/Users/jakub/repos/dcsdd_compressed.pdf`).

User rulings: full DCSDD (samples-only, faithful to the paper — not classic DC with
exact Hermite data); reconstruction runs async after each edit; shaded mesh is the
primary view with wireframe only for the selected node; normal-colored debug shading;
delete switches to camera mode.

## 1. Delete op

- RadialMenu gains a fourth button (`xmark` SF symbol) alongside move/scale/op-toggle.
- Core: `Editor::deleteSelectedNode()` — removes the node from `SceneDocument`, clears
  selection, hides gizmo, dirties the line buffer, triggers reconstruction.
- Swift: after the call, `setMode(.camera)`. Deletion is permanent (no undo system).

## 2. SDF evaluation & sampling — `core/src/sdf.{h,cpp}`

- Per-node local SDFs: exact box SDF with per-axis half-extents `scale·0.5` (exact under
  nonuniform scale); sphere under nonuniform scale = ellipsoid via iq's approximation
  (`k0·(k0−1)/k1`). Points transformed by `inverse(world_from_local)`.
- CSG combine in node order: Add → `min(d, d_node)`, Subtract → `max(d, −d_node)`.
  Known approximation: min/max is not a true distance near intersection curves; the
  paper demonstrates robustness to comparable noise (Table 1). Documented, accepted.
- Sampling domain: scene AABB inflated to a cube, +10% margin, N=64 samples per axis.
- Everything downstream consumes **grid samples only** — the analytic SDF is never
  queried past sampling. This keeps the implementation honest to the paper's setting.

## 3. DCSDD reconstruction — `core/src/dcsdd.{h,cpp}`

Pure CPU, no Metal types, headless-testable. Direct mapping of the paper's Algorithm 1:

User ruling: CPU now, with a GPU-ready data layout — flat index-based buffers
throughout (no pointer graphs; hash maps allowed only during construction, never in
the steady-state representation). This keeps a later Metal compute port mechanical,
and lets that port be validated against this CPU implementation as the reference.

- **Init**: interesting edges = sign changes; Hermite point via linear interpolation
  (Eq. 1); Hermite normal via averaged trilinear-interpolant gradients (Eq. 2); cell
  vertex at the centroid of its edges' Hermite points (Eq. 3).
- **Outer loop** (default 30 iters): build the global quad mesh (one quad per
  interesting edge, joining the 4 containing cells' vertices); triangulate by first
  diagonal; compute face intersection points **p** per quad; assign each narrow-band
  sample to the cell containing its closest point on the global mesh; run the inner
  loop; update each edge's Hermite data by PCA over the 4 sharing cells' vertices —
  best-fit plane normal + edge intersection, blended with update weight (Eq. 7).
- **Inner loop** (default 30 iters, per cell, parallelizable): local triangle mesh from
  Hermite points, face intersection points, and the cell vertex (Fig. 8); distance
  energy with radial linearization toward sphere centers (Eq. 9), Hermite energy
  (Eq. 5), L2 regularizer; solve the 3×3 normal equations by accumulating QᵀQ and Qᵀc
  directly (Eqs. 12–15) — no matrix library. Early exit on `‖x_{r+1} − x_r‖ < tol`.
  Vertices may escape their cells (paper: helps sharp features; Fig. 15).
- **Deviations from the paper's implementation** (not the algorithm):
  - Narrow band: only samples with `|s| < 2·cell_diagonal` enter the distance energy
    (sanctioned by the paper for cost control).
  - Closest-point queries via uniform-grid binning of triangles into their cells with
    expanding-ring search, instead of libigl AABB trees — triangles are cell-sized, so
    the grid is the natural accelerator.
  - Sphere-outlier rejection as in the paper: skip samples whose distance to the mesh
    exceeds `cell_diagonal + |s_i|`.
- All tunables in one `DcsddConfig` struct: `resolution=64`, `outer_iters=30`,
  `inner_iters=30`, `w_H=0.02`, `w_u=0.5`, `mu=0.05`, `tol`, narrow-band factor. Paper
  defaults noted in comments (100³, 100/100).
- Output: quad mesh → triangle list (two per quad) with facet normals, in plain
  `std::vector`s (positions + normals, non-indexed is fine at this size).

## 4. Async job model

- Triggers: spawn, drag release, scale release, op toggle, delete. Never during an
  active drag.
- On trigger: snapshot `SceneDocument` (cheap vector copy), enqueue on one background
  `std::thread`. One job in flight + a dirty flag; edits during a run coalesce into
  exactly one follow-up run with a fresh snapshot.
- Delivery: mutex-guarded mailbox holding the finished CPU mesh; `Editor::render()`
  (main thread) checks it, creates the MTL vertex buffer there, swaps atomically. No
  Metal work off the main thread. Empty reconstruction (empty scene) clears the mesh.

## 5. Rendering

- Render pass gains a depth attachment (Depth32Float texture, recreated on resize).
- New triangle PSO: depth test/write on; normal-colored debug shading
  `color = 0.5·(n+1)` with facet normals, so recovered sharp edges read visually.
- Existing line + gizmo PSOs: declare the matching depth attachment format, but use a
  depth-stencil state with compare Always / write off — lines stay painter's-order on
  top, exactly as today. Draw order: mesh → scene lines → gizmo.
- Wireframe policy: once a mesh exists, `build_scene_lines` emits only the selected
  node; with no mesh yet (or empty scene), all nodes as today.

## 6. Interop & Swift

- `ShapeshifterCore.h` addition: `deleteSelectedNode()`. Nothing else — reconstruction
  triggers internally on mutations and `render()` picks up results.
- Swift: RadialMenu fourth button; view-model `deleteSelected()` → core call +
  `setMode(.camera)`.

## 7. Testing (doctest, headless)

Independently-derived (numpy) literals, per project policy:

- SDF eval: box under nonuniform scale, ellipsoid approximation, CSG add/subtract.
- Hermite init: Eq. 1 crossings and Eq. 2 normals vs. numpy trilinear gradients.
- Centroid init (Eq. 3); quadratic solve vs. numpy least-squares on a pinned system
  (Eqs. 12–15); PCA Hermite update (Eq. 7) on pinned configurations.
- Acceptance: sphere → every output vertex within tolerance of `|p| = r`; the paper's
  Fig. 16 test — a box at low resolution (8³) recovers sharp corners (a vertex within ε
  of each true corner), the case where marching cubes and estimated-Hermite DC fail.
- Job coalescing logic unit-tested with a fake runner (no threads in assertions).

## 8. Out of scope

Materials/lighting beyond normal-debug, raymarching, mesh export, undo, progress UI,
GPU-side reconstruction, adaptive/octree grids, remeshing during drags.
