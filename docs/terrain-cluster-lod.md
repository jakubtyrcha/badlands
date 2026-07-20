# Terrain cluster-LOD (Nanite-style, hardware raster)

A restricted, grid-aware Nanite for terrain: a cluster DAG built at map load,
boundary-locked QEM simplification, and per-frame screen-space-error cut
selection — **seamless LOD by construction** (no cracks, no skirts, no
neighbor-LOD rules), hardware rasterization only. First integration is
`badlands_mapview`; the game app is a later target.

Design SSOT (algorithm, why it's seamless, the four load-bearing invariants):
`docs/superpowers/specs/2026-07-19-terrain-cluster-lod-design.md`. This note is
the shipped-state summary: what's live, how to drive it, the measured numbers,
and the recorded follow-ups.

## What it is

- **One DAG per map.** After mapgen composes the heightmap, `BuildTerrainClusterDag`
  tessellates it into 8×8-quad leaf tiles (128 tris each), then repeatedly
  {group 2×2 → weld → lock the shared group boundary → `meshopt_simplifyWithAttributes`
  to ~50% → median-split into 2 clusters → record error+sphere} until one root
  remains. ~12–13 levels, ~8k clusters, ~1.05M tris for a 512² map.
- **Runtime is CPU selection + per-cluster `DrawIndexed`.** `SelectClusters` is a
  flat pass over all clusters (no traversal); the render pass culls each selected
  range against its own frustum. One `StaticTexturedMeshComponent` holds the whole
  shared vertex/index buffer; a `MeshDrawRangesComponent` carries the current cut.
- **Per-vertex data:** position, normal, RGB biome color (baked from the palette),
  biome id. Coarse LODs shade from vertex color. Layout `kTerrainCluster`, 32 B/vertex.

## The two invariants (why it's crack-free)

1. **Selection can only produce a valid cut.** Along any leaf→root chain, errors
   are monotone (forced by `max`) and LOD spheres nest, so *projected* error is
   monotone and crosses τ exactly once. The rule `proj(own) ≤ τ AND proj(parent) > τ`
   selects exactly the crossing cluster — every point covered once, no ancestor+
   descendant overlap. All siblings of a group share one error+sphere, so they
   flip in lockstep.
2. **Any valid cut is geometrically crack-free.** A boundary curve is only modified
   by a simplification whose group contains *both* its sides; a seam vertex is
   passed as locked to both adjacent groups, so both sides keep it bitwise-identically.
   Adjacent drawn clusters at different LODs carry bitwise-equal boundary vertex sets
   → no cracks, no T-junctions, zero runtime coordination.

Both are pinned by the Catch2 suite (`terrain_clusters_tests.cpp`): error
monotonicity, sphere nesting/sharing, bitwise seam agreement + completeness, and
cut validity (antichain + exact cover) over several cameras/taus.

## Constants and tau

- **Partition/budget constants** are `constexpr` in
  `src/game/geometry/terrain_clusters.hpp` — `kTileQuads` (8), `kGroupDim` (2),
  `kClusterTriBudget` (128), `kGroupSplitCount` (2), `kSimplifyTargetRatio` (0.5).
  No bare partition literals in the build; `TerrainClusterParams` defaults to these,
  so a test (or a caller) can reshape the build by overriding them.
- **τ (`tau_px`)** is the screen-space-error budget in **pixels**: a cluster is
  coarse enough to draw when its LOD error projects to ≤ τ px at the current camera
  distance. Lower τ = finer/more clusters. Default 1.5 px.
  `proj(e, sphere) = e · (screen_h / (2·tan(fov/2))) / max(dist − radius, ε)`;
  camera inside the sphere ⇒ +∞ (forces refinement), zero-error leaf ⇒ 0.

## Color path — palette bytes are written RAW (do NOT linearize)

Verified end-to-end (M4): the G-buffer albedo target is linear `BGRA8Unorm`, and
legacy `terrain_blend` loads its albedo textures as plain `RGBA8Unorm` with **no**
sRGB→linear decode — both paths treat their 8-bit albedo as linear and share the
same deferred lighting + tonemap (`linear_to_srgb` at resolve). So the biome
palette bytes are packed raw (`PackU8x4`, byte/255) with no decode. **Linearizing
the palette would make cluster terrain darker than legacy** — a mismatch. The
cluster-vs-legacy look difference is content (flat vertex color vs PBR textures —
a recorded follow-up), not a gamma bug. See the note at the palette bake in
`terrain_clusters.cpp`.

## Toggles and flags

Runtime (mapview ImGui "Map" panel, cluster terrain live):
- **Cluster terrain** checkbox — A/B against the legacy fixed-subdiv chunks.
- **LOD tau (px)** slider — the τ budget (0.25–16).
- **Debug tint** combo — `Shaded (biome color)` / `Triangle hash` (per-triangle
  position hash of `meta.y`, NOT a stable per-cluster color — deferred) / `LOD level`
  (hue wheel). Drives `debug_params.x` in `terrain_cluster.wesl` via a live per-draw
  override.
- Cut stats: selected cluster count, count in the camera frustum, whole-cut tris,
  CPU selection µs, and a per-level histogram.

Headless CLI (`badlands_mapview`):
- `--legacy-terrain` — render the legacy chunks instead of cluster terrain.
- `--camera-height H` — starting camera height in metres (near/far framing).
- `--lod-tau T` — seed τ in pixels (default 1.5).
- `--lod-tint N` — seed tint mode (0 shaded / 1 triangle hash / 2 LOD level).
- `--serial-build` — build the DAG single-threaded (perf A/B; bit-identical output).

## Build parallelism + determinism gate

The per-group weld+simplify+split within each level runs over the shared thread
pool (`ParallelFor`, `src/core/parallel.hpp`). Only that *pure* per-group
computation is concurrent; cluster/group emission stays serial in a fixed
orz-major order, so the output DAG is **bit-identical to a serial build**. This is
non-negotiable and pinned by a determinism test that builds a map both ways
(`params.parallel_build` off/on) and compares vertices, indices, cluster, group,
and group-children arrays byte-for-byte (plus a parallel-vs-parallel reproducibility
check). `--serial-build` exposes the same toggle from the CLI.

## Measured perf (seed 2, 512×512 map, τ 1.5, capture height 900 px)

Build time (from the stats log, this machine, 17 workers):

| build     | time      |
|-----------|-----------|
| serial    | ~2183 ms  |
| parallel  | ~496 ms   |

~4.4× speedup; both produce the identical cut (908 clusters / 116146 tris near),
confirming the determinism gate at the app level too. 512² map: 13 levels,
~8.2k clusters, ~1.05M tris total.

Per-frame runtime — `SelectClusters` ≈ **0.95–1.0 ms** (flat pass over ~8.2k
clusters). Draw counts by camera (selected cut → survive the **camera-pass**
frustum cull; the shadow pass uses the light frustum and is not instrumented):

| camera                | cut clusters | in camera frustum | cut tris |
|-----------------------|--------------|-------------------|----------|
| near (height 25)      | 880          | 11                | 112560   |
| mid (height 120, τ 8) | 350          | 111               | 44729    |
| far/overhead (500)    | 504          | 504               | 64429    |

The cut is the whole map (frustum-independent, so the shadow pass draws off-screen
casters); the camera pass draws only the frustum-visible subset — near-ground
shots see a small slice, overhead sees the whole map. Higher τ and distance both
shrink the cut, as expected.

## Recorded follow-ups (not built)

- **GPU-driven selection + `DrawIndexedIndirect`** — replace the CPU flat pass +
  per-cluster `DrawIndexed` with compute selection and indirect draws.
- **Biome texture-array sampling at near LOD** — biome id is already in the vertex
  layout; near clusters could sample the PBR terrain arrays instead of flat vertex
  color (the current v1 look difference vs legacy).
- **Per-level normal re-derivation** — coarse LODs carry the leaf normals, so 1 m
  detail can shimmer; re-derive normals per level to fix it.
- **Stable per-cluster debug tint** — needs a per-range user tag (`MeshDrawRange`) +
  `firstInstance` in the ranged-draw loop, i.e. an engine-interface change (approval
  required). Today's tint is a per-triangle position hash.
- **DAG disk caching** — the DAG is always rebuilt at load. If map sizes grow enough
  that build time matters, cache the built DAG. Explicitly out of scope now.
- **Direct cross-level seam test** — the suite proves seam agreement + completeness
  between same-level neighbors; add a test that, for each group, asserts its locked
  boundary vertex records appear bitwise-identically in its output clusters (the
  parent-carries-the-locked-curve half of invariant 2, checked directly rather than
  transitively).
- **Build-loop progress guard** — if a level's simplify yields no triangle reduction,
  force `num_out = 1` (or break) so the reduction can't stall; also covers the
  theoretical `meshopt` returns-0-triangles case. Today the median split + region
  reduction always makes progress on the tested maps, but the guard would make the
  loop robust by construction.
