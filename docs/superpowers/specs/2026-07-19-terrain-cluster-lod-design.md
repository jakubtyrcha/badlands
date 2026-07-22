# Nanite-style cluster-LOD terrain (hardware raster)

## Context

Terrain rendering today (mapview) tessellates fixed 16×16-block chunks at a fixed `subdiv` — triangle count scales with map area, not with what's on screen. Goal: render large terrain cheaply by decimating the mesh where it's flat/far and keeping detail on ridges/cliffs and material (biome) boundaries, with **seamless LOD by construction** (no cracks, no skirts, no neighbor-LOD rules).

Approach: a **restricted, grid-aware Nanite**: cluster DAG + boundary-locked QEM simplification + screen-space-error cut selection — but **hardware rasterization only**, targeting *large* triangles (16–32px density ceiling). Explicitly out of scope (this is ~80% of real Nanite's complexity): software rasterizer, visibility buffer, streaming, occlusion/HiZ, METIS partitioning, virtual shadow maps.

## Decisions (user-approved)

- **One DAG per map** — whole heightmap is one build; level-0 clusters are grid tiles. No interior locked perimeters → no dense seams at low LOD.
- **meshoptimizer as C++ submodule** in `third_party/` (`meshopt_simplifyWithAttributes`: QEM + attribute error + locked verts). No FFI seam.
- **Build at map generation/load**, right after mapgen composes the heightmap. No baked assets, no disk caching — always rebuilt. (If build time ever becomes a real cost, caching the built DAG is a possible *future* optimization; explicitly out of scope for this plan.)
- **Runtime v1 = CPU selection + per-cluster `DrawIndexed`** from shared vertex/index buffers (~600–800 draws/frame expected). GPU compute selection + indirect draws deferred.
- **Per-vertex data:** position, normal, RGB color (baked from biome palette — coarse LODs shade from vertex color), biome id. New vertex layout + terrain shader variant.
- **First integration target: `badlands_mapview`**; game app later.

## Algorithm summary (for the implementer)

Build (per map, after mapgen):
1. Tessellate heightmap → full-res leaf mesh; bake vertex color + biome id.
2. Level 0: partition into grid tiles of 8×8 quads ≈ 128 tris = 1 cluster.
3. Loop until one group remains: **group 2×2 tiles → merge tris → lock group outer-boundary verts → simplify interior to ~50% (QEM w/ attributes) → split into 2 clusters → record error**.
4. Error monotonicity: `cluster.error = max(group simplification error, children errors)`. All clusters born of one group share the same error and the same LOD bounding sphere (encloses children's spheres) → group-consistent refinement decisions.
5. DAG: each child has 2 parents, each parent 4 children (the re-split crossing old boundaries is what lets boundaries alternate: group-internal boundaries dissolve this level, group-external ones dissolve next level).

Runtime (per frame, CPU):
- For each cluster: draw iff `proj(own_error) ≤ τ` **and** `proj(parent_group_error) > τ`, where `proj(e) = e · (screen_h/2) / (tan(fov/2) · dist(camera, lod_sphere))`. τ ≈ 1–2px.
- Frustum-cull selected clusters (existing `Frustum`), one `DrawIndexed` per cluster (vertex/index ranges into shared buffers).
- Crack-freeness: boundary edges only change when both sides are in the same group, so any valid cut (no ancestor+descendant rendered together) has bitwise-identical shared boundary verts — no runtime constraint between neighbors.

## Why this is seamless — the two load-bearing mechanisms

**How locked boundaries are selected at each level.** There is no separate selection step — the locked set falls out of the grouping: *a vertex is locked iff it lies on the boundary between two different groups of the current level.* Everything interior to a group — including boundaries between the group's own member clusters — is unlocked. (Full Nanite gets its groups from METIS min-cut partitioning; our grid variant is deterministic.) Concrete schedule on the grid: level 0 groups are 16 m squares → locked = grid lines at multiples of 16; lines at odd multiples of 8 are group-interior and decimate this level. Level 1 groups are 32 m → lines at multiples of 16 (locked last level) are now interior and decimate, except multiples of 32. In general a line at `x = 8·2^j·(odd)` stays locked for levels 0..j and dissolves at level j+1 — every boundary is locked finitely long, nothing stays dense forever (map perimeter excepted: no neighbor at any level, deliberately left unlocked). Two mechanical details: (1) a seam vertex is passed as locked to *both* adjacent groups' simplify calls, so after independent (parallel) simplification both sides retain it bitwise-identically; (2) the re-split into 2 clusters deliberately ignores old cluster boundaries (median cut) — that's why it's a DAG and why next level's "interior" differs from this level's; the boundary alternation is a consequence, not an added trick.

**How rendering avoids seams between neighboring LODs.** It doesn't constrain neighbors at all — no adjacency data at runtime, no ±1-level rule, no stitching, no skirts. Two mechanisms combine:
1. *The selection rule can only produce a valid cut.* Along any leaf→root ancestor chain, errors are monotonically non-decreasing (forced by `max`) and LOD spheres nest, so **projected** error is also monotone from any camera position; it crosses τ exactly once, and the rule (`proj(own) ≤ τ AND proj(parent_group) > τ`) selects exactly the crossing cluster. Every terrain point is covered exactly once — no holes, no ancestor+descendant overlap. Group-shared error+sphere ⇒ all children of a group evaluate the identical parent test and agree unanimously.
2. *Any valid cut is geometrically crack-free* — build invariant: a boundary curve is only modified by a simplification whose group contains both of its sides. For adjacent drawn clusters A (level L) and B (level M > L) sharing a curve with vertex set V at level L: each step of B's ancestry from L to M either kept the curve locked (V preserved bitwise) or dissolved it inside a group — but the latter would make that group's outputs ancestors of A, which cut validity forbids alongside A. So B carries exactly V on the curve, bitwise equal to A's. Identical vertex sets ⇒ no cracks, no T-junctions, zero runtime coordination.

Caveat that shapes the implementation: mechanism 1 leans on monotone *projected* error — hence parents must enclose children's spheres and siblings must share them. Tight per-cluster spheres would break the cut from some camera angles. The Catch2 tests in Verification pin all four load-bearing properties (error monotonicity, sphere nesting/sharing, bitwise seam equality, cut validity).

## Engine-interface changes (require explicit approval — CLAUDE.md)

Approving this plan approves exactly these three engine/core additions; everything else lives in `src/game/geometry/`, `src/mapview/`, `shaders/material/`:
1. `GeometryType::kTerrainCluster` in `src/core/geometry_type.hpp` + mapping in `src/engine/rendering/material/material.cpp` (~line 35-52).
2. `VertexLayout::kTerrainCluster` in `src/engine/rendering/vertex_layout.{hpp,cpp}`: `Float32x3 pos @0, Float32x3 normal @1, Unorm8x4 color @2 (offset 24), Uint8x4 meta @3 (offset 28)`, stride 32 B.
3. `MeshDrawRange { first_index, index_count, Aabb bounds }` + `MeshDrawRangesComponent` in `src/engine/rendering/components/mesh_components.hpp`, and a ~25-line ranged-draw loop in `render_textured_mesh.cpp`: if present, per range → cull `bounds.TransformedBy(model)` against the pass frustum → `pass.DrawIndexed(range.index_count, 1, range.first_index)` (signature already supports `first_index`, `render_pass_context.hpp:51`). Game-agnostic ("multi-range indexed submesh with per-range bounds"). No `UniformData`/`frame.wesl` change, no new pass, no `SceneRenderer` change.

Rejected integration alternatives: N dynamic entities (the lazy-upload pass would re-upload churn every frame); a game-layer render hook (no plugin point exists in `SceneRenderer::Render` — adding one is a bigger interface change than the ranges component).

## Data structures — new `src/game/geometry/terrain_clusters.{hpp,cpp}`

Flat parallel arrays, indices not pointers:
- Per cluster: `first_index/index_count` (range into shared index buffer), `first_vertex/vertex_count` (debug/tests), world `Aabb bounds`, `own_group` (group that produced it; leaves = none, error 0), `parent_group` (group that consumed it; roots = none, error +inf), `level`.
- Per group: `group_error_m` (monotonic, world meters), `group_sphere` (vec4; encloses children's spheres AND the group AABB → well-formed cut), child span into a flat `group_children` array.
- Shared geometry: interleaved `vertices` (8 floats = 32 B: pos 3f, normal 3f, color Unorm8x4 packed as 1 float, meta Uint8x4 packed as 1 float: `{biome_id, cluster_hash_byte, lod_level, 0}`), global `indices` (u32). Vertices are never shared across clusters (boundary verts duplicated bitwise-identically) — that's what makes per-vertex cluster/level debug-tint legal. Reuse `PackU8x4` from `terrain_mesh.cpp`.

Selection is a pure function (unit-testable, no GPU):
```cpp
void SelectClusters(const TerrainClusterDag&, glm::vec3 cam_pos, float fov_deg,
                    float screen_h_px, float tau_px, std::vector<uint32_t>& out);
// selected iff proj(own_group.error) <= tau AND proj(parent_group.error) > tau
// proj(e, sphere) = e * (screen_h / (2*tan(fov/2))) / max(dist(cam, center) - radius, eps)
// camera inside sphere -> +inf (forces refinement). Flat pass over ~8k clusters, no traversal.
```
No frustum cull inside the selector — the cut covers the whole map; the *pass* culls per-range against its own frustum, so the shadow pass (light frustum) still draws casters off-screen for the camera.

## Build pipeline details

`BuildTerrainClusterDag(heightmap, biomes, params)` called in `MapViewView::Initialize` right after `run_pipeline` (`map_view_view.cpp:57`).

**Compile-time constants — no hardcoded partition literals.** All partition/budget numbers live as named `constexpr` in `terrain_clusters.hpp` (pattern of `mapgen_constants.hpp`), used everywhere the algorithm needs them — never inline literals:
```cpp
constexpr int kTileQuads = 8;              // leaf tile edge, in quads
constexpr int kGroupDim = 2;               // group = kGroupDim x kGroupDim children
constexpr int kClusterTriBudget = 2 * kTileQuads * kTileQuads;  // = 128
constexpr int kGroupSplitCount = 2;        // clusters emitted per simplified group
constexpr float kSimplifyTargetRatio =
    float(kGroupSplitCount) / (kGroupDim * kGroupDim);           // = 0.5
static_assert(kGroupDim * kGroupDim * kClusterTriBudget % kGroupSplitCount == 0);
```
Derived values (verts per tile, group footprint at level L = `kTileQuads << L`, the locked-line schedule) are computed from these, so changing e.g. `kTileQuads` to 16 or `kGroupDim` reshapes the whole build consistently.

- **Leaves:** vertex grid at 1 m/sample, positions `(i, SampleHeight(i,j), j)`; per-vertex normal via `NormalAt` (hoist from `terrain_mesh.cpp` anon namespace — game-layer change), color = `kBiomePalette[biome]` (`mapgen/biomes.hpp`), biome id. Tile = 8×8 quads, 2-tri split (not the 4-tri X-split) → 128 tris, 81 verts. 512×512 map → 64×64 = 4096 leaves.
- **Loop until 1 cluster:** group 2×2 (from L1 up: regions hold 2 clusters, group = 2 adjacent regions, alternating pair axis X/Z) → weld children's verts by exact float bit-pattern (valid: meshopt never moves vertices; all positions come from the same `SampleHeight` evaluations) → lock via per-vertex `vertex_lock` (NOT `meshopt_SimplifyLockBorder`): lock verts on group-footprint lines *shared with another group*; map-perimeter verts stay UNLOCKED (no neighbor → no crack; locking would pin the 2 k-vert perimeter at 1 m density forever and blow coarse budgets; silhouette shrink is error-bounded) → `meshopt_simplifyWithAttributes(..., attrs = normal(3)+colorRGB(3), weights ~0.5 each (tunable), vertex_lock, target = n/2, FLT_MAX, meshopt_SimplifyErrorAbsolute, &result_error)` — absolute mode makes `result_error` world meters; biome id is NOT an attribute (collapse-onto-existing-vertices means survivors keep valid original ids) → **split into 2** by sorting simplified tris by centroid along the longer footprint axis, cut at median (deterministic, compact; split quality only affects cull tightness, not correctness) → record group `{error = max(result_error, children's errors), sphere}`.
- Parallelize groups within a level via `ParallelFor` (`src/core/parallel.hpp`).
- **Non-power-of-two:** `tiles = ceil(extent/8)`, last tile clamps; odd region counts allow 2-3-child trailing groups (emit 1 parent if output ≤ ~1.5× budget, else 2). Unit-test 100×60.
- Expected (512²): ~12-13 levels, ~8.2 k clusters, ~1.05 M tris total, ~22 MB VB + ~12.6 MB IB, build ≲1-2 s single-threaded.

## Runtime integration (mapview)

- `Initialize`: build DAG; ONE entity: `StaticTexturedMeshComponent{vertices, indices, geometry_type=kTerrainCluster, transform=identity}` (absolute world coords — camera-offset rebase at `render_textured_mesh.cpp:137` works unchanged), game-built material factory, `StaticMeshAabbComponent` = whole-map AABB, empty `MeshDrawRangesComponent`. Keep legacy chunk path behind a toggle for A/B.
- `Update` (after camera update): `SelectClusters(...)` → rewrite `ranges` (bounds from `dag.bounds`). Buffers upload once via the existing lazy path.
- **Material factory (game side, no engine edit):** mirror `material_library.cpp:48-58` — `FactoryDescriptor{shader_name="terrain_cluster", shader_path="material/terrain_cluster.wesl", fs_entry="fs_gbuffer", pass=kDeferred, geometry={kTerrainCluster}, color_formats=GBuffer MRT, depth=GBuffer depth}` → `BuildMaterialInstanceFactory` (public API, verified `material_instance_factory.hpp:96`).

## Shader — new `shaders/material/terrain_cluster.wesl`

Modeled on `terrain_blend.wesl`: `@if(shadow_pass)` depth-only variant; per-object `TerrainClusterUniforms{modelMatrix, debug_params}` (group 1). VS passes worldNormal, color, `@interpolate(flat)` meta (safe — all verts of a tri belong to one cluster). FS `fs_gbuffer`: albedo = vertex color (v1: always; biome texture sampling is an explicit follow-up — biome id already in the layout), octahedral normal, fixed roughness/AO. `debug_params.x`: 0 normal / 1 tint-by-cluster / 2 tint-by-level, driven via `fmc.params.uniform_overrides` (transferred per draw, `render_textured_mesh.cpp:145`).

## CMake / dependency

- `git submodule add https://github.com/zeux/meshoptimizer third_party/meshoptimizer` pinned to a release tag (v0.24); `add_subdirectory` in root `CMakeLists.txt` (exports target `meshoptimizer`).
- Add `terrain_clusters.cpp` to `badlands_mapview` sources (`CMakeLists.txt:322-327`) + link `meshoptimizer`; same for the new test target.

## Verification

- **Catch2 target `badlands_terrain_cluster_tests`** (pure CPU, synthetic `Field2D`, pattern of `badlands_mapgen_tests`): (1) monotonic errors; (2) siblings share error+sphere, parent sphere contains children's; (3) crack-freeness — shared-footprint-edge vertex sets bitwise-equal between same-level neighbors, and each group's locked boundary verts appear bitwise-identical in its parent clusters; (4) cut validity — for several cameras/taus, no ancestor+descendant both selected, every leaf has exactly one selected ancestor-or-self; (5) grid arithmetic incl. 100×60; (6) **constants validation** — build a DAG with non-default constants (test compiles a variant with e.g. `kTileQuads=4`, or the params struct carries the values with the constexprs as defaults) and assert tests 1-5 still hold; plus a review step in M1: grep the build code for magic numbers `8`/`2`/`128` — every partition value must trace to the named constants.
- **ImGui (MapViewView::DrawUI):** cluster-terrain toggle, `tau_px` slider, tint combo, stats (selected clusters, drawn tris, per-level histogram).
- **Headless:** mapview `--screenshot` with new `--cluster-terrain --camera-height H --lod-tau T` flags → screenshots at near + far heights, vs legacy chunks; tinted variants for boundary inspection.
- End-to-end: `cmake --build build && ctest --test-dir build`, then `./build/badlands_mapview --seed 2 --resolution 500x500` and eyeball seams while flying.

## Milestones

- **M1 — Build + stats** (no rendering): submodule + CMake; DAG build; mapview logs per-level counts/budgets/errors; tests 1-3, 5 green.
- **M2 — Render leaves**: engine additions (GeometryType/VertexLayout/ranges component) + shader + factory; all leaf clusters as ranges, no LOD; screenshot vs legacy chunks.
- **M3 — Full LOD**: `SelectClusters` wired in; tau slider; test 4; verify ~600-800 gbuffer draws + shadow draws, two-distance screenshots.
- **M4 — Shading/debug polish**: tints, sRGB/palette check, attribute-weight tuning, seam eyeball pass.
- **M5 — Hardening**: full suite in ctest, `ParallelFor` build, perf numbers, docs note. Follow-ups recorded (not built): GPU-driven selection/indirect, biome texture sampling at near LOD, per-level normal re-derivation.

## Risks (accepted for v1)

- Map-edge unlock → coarse silhouette may pull slightly inward (error-bounded; alternative blows coarse budgets).
- Locked-seam density can push some parents past 128 tris (monitor in M1 stats).
- meshopt attribute error blends into `result_error` → slightly conservative LOD switching (acceptable; can do a position-only error query later).
- v1 vertex-color shading is flat vs today's textured near-field (textures are the recorded follow-up).
- Coarse normals keep 1 m-scale detail → possible shimmer (follow-up: per-level normal re-derivation).
