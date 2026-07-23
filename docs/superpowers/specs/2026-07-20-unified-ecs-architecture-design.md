# Badlands — System Architecture (unified ECS)

> Design document. Establishes the layer boundaries and the data-only interfaces
> between them around a single shared entt world, resolves where terrain LOD
> belongs, defines the low-level GPU vs high-level Renderer split, and works the
> terrain pipeline end-to-end. Explore/prototype phase — the goal is systems that
> extend cleanly, grounded in the current code (paths cited). On approval this is
> committed to `docs/superpowers/specs/`.

## What prompted this

Making terrain render through one Nanite-style LOD pipeline in both the map tool
and the game forced the architecture into the open. Three things are wrong today:
terrain **LOD selection lives in the game layer** (`src/game/map/cluster_terrain.*`,
`src/game/geometry/terrain_clusters.*` — a rendering decision in the wrong place);
the scene→ECS sync **rebuilds the entire registry every frame** (`SceneGraph::SyncToRegistry`
does `registry.clear()` + deep-copies every mesh + forces GPU re-upload, `scene_graph.cpp:242-319`);
and there are **two entt registries** (sim behind a data-only C ABI, render built
from snapshots) even though an "object" should just be one entity.

This document settles the target the team wants: **one unified entt world** as the
data bus, a clean sim/render/world-gen/app layering with data-only seams, and the
terrain LOD pipeline sitting in the Renderer where it belongs.

## The spine: one unified entt world

**One `entt::registry` is the universal data bus.** An *object is an entity*; its
data lives in components (`Transform`, rendering components, game/sim components,
`TerrainLodComponent`, …). Every layer reads and writes components on that one
world; the interface *between* layers is **data (components)** — never a cross-copy
boundary. This is the ECS philosophy taken at face value, and it collapses three
things that exist today:

- **The sim's separate registry + data-only C ABI dissolve.** `badlands_game_lib` keeps existing as a library for modularity/testability, but its interface changes from the POD-snapshot C ABI (`game/include/badlands_game.h`: `game_state`/`game_buildings`/`game_dispatch`) to **C++ system functions that operate on the shared `entt::registry&`**. No snapshot copy; sim and rendering see the same entities. Observation is via **entt views/groups** (`registry.view<Components…>()`), not a facade — a system names only the components it needs, so the renderer's views never touch sim components and vice-versa. Decoupling is a consequence of *which components each system selects*, backed by a grep gate (render code includes no game-component headers). Structural change ("dirty") uses entt's `on_construct/on_update/on_destroy` signals. (The Rust FFIs — noiser brains, `nav`, `assets`, `wesl` — remain as *language* seams, data-only, unchanged.)

  **Why the boundary existed (and why it's safe to remove):** it was a **cross-language bridge, now obsolete** — not a deliberate same-language isolation. badlands began as a *Rust-hosted* (winit/wgpu) app driving a *C++/EnTT* sim, so a data-only C ABI was required across the Rust↔C++ seam (`docs/superpowers/specs/2026-07-14-badlands-cpp-replatform-design.md:8-12`; sim landed in commit `447283e`). The replatform moved host+renderer to C++ (Dawn/SDL3), so the language reason is gone; the C ABI is now a C++↔C++ vestige. Its accreted secondary value (compile-time sim/render decoupling; headless/mockable sim testing) survives unification in weaker-but-sufficient form: sim systems stay unit-testable on a bare registry, and decoupling moves from compile-time impossibility to view-selection + grep discipline. The genuine loss — a hard structural wall and any out-of-process sim — is not wanted here.
- **The retained `SceneGraph` + `SyncToRegistry` are removed.** The registry *is* the scene: render entities persist across frames; "dirtying the scene" = structural component changes (new object / new mesh / new material), tracked with entt's `on_construct/on_update/on_destroy` signals; the Renderer iterates render-component **views** each frame. No per-frame flatten, no per-frame vertex re-upload.
- **Transforms are flat and live.** Each entity holds its own world `Transform`; whoever moves it (a sim system, the app) writes that component; the Renderer reads it live every frame. No hierarchy composition at runtime, no transform sync, no transform-driven "dirty."

## The layers

Dependencies point **downward only**; the only cross-layer currency is components
on the shared world (plus `MapData` for world-gen).

### A. Game simulation
- **Is:** systems for placement, movement, brains/AI, combat — `badlands_game_lib` (`game/src/*.cpp`), now operating on the **shared** registry via C++ APIs taking `entt::registry&`.
- **Reads/writes:** game components (`Position`, `Health`, `MoveTarget`, `Team`, …, `game/src/components.h`) + `Transform` (for things it moves) + world data via `MapData` **queries** (`HeightAt`/`BiomesAt`, `map_data.hpp:90-95`).
- **Rules:** as a rule of thumb the sim **does not look at rendering** — it never reads render components or does rendering logic (LOD, culling, draw order). It **may** write game-driven parameters the renderer consumes (e.g. a time-of-day system writing the light direction/color component) — as data. Brains stay noiser scripts (`game/src/brain.h`).

### B. Rendering
Decoupled from game logic; sees only render components + transforms (which other
layers write, but the interface is DATA). Deals with meshes, materials, lighting;
supports **blockout and detailed** modes. Split (see E) into:
- **B1 — GPU backend (low-level).** Device/queue/surface, pipeline generation + shader reflection (`wesl`), bind groups, pass *execution*, frame/pass context, gbuffer, shadow/IBL/mip/fog bakes, GPU mesh upload. **Consumes** render components (`StaticTexturedMeshComponent` incl. indices, `MaterialFactoryComponent`, `MeshDrawRangesComponent`, `Transform`, lights) and draws them. Knows *how to render a mesh*; never decides *what*/at what detail. Today: `src/engine/rendering/{gpu_context,shader/,context/,passes/,material/,gbuffer,…}`.
- **B2 — Renderer (high-level).** Turns *the world + camera* into draw-ready components: per-frame **LOD picking** (build the terrain cluster DAG; each frame select the cut and update the draw-range component), **culling**, material selection, and reacting to structural component changes via entt signals. It **updates** the components B1 consumes and iterates render-component views. Knows scene + camera + rendering data — **not** game logic or world-gen types.

### C. World generation
- **Is:** produces *the map* — a heightmap + per-biome coverage. Procedural (`src/mapgen/`, `run_pipeline`) or predefined/authored (deterministic for testing). Input random (procgen) or predefined per app/test need.
- **Interface:** the frozen **`MapData`** contract (`src/game/map/map_data.hpp`, "no triangles, no materials, no render types"), two faces — game-logic **queries** and the **raw lattice** (render input). **Map-only for now:** `MapData` stays a single *immutable lattice*; we do not add feature-list / region / mutation surfaces yet (those are the extension points if/when world-gen grows).
- **Rules:** depends on `src/core` only; consumed by both A (queries) and B2 (lattice).

### D. Apps
Own the single `entt::registry`, choose a world-gen source, register the sim
systems, seed render entities, and drive the frame (**sim tick → Renderer**). Each
app differs only in sources + modes:

| App | World gen | Sim | Rendering |
|---|---|---|---|
| **mapview** | procedural/authored mapgen | stub time sim (`SimClock`) | rendering-focused; cluster-LOD terrain; debug tints |
| **ai_sandbox** | deterministic map | **game-sim** focused (target) | **blockout** debug |
| **game** | symbolic → later procedural | **real** sim | **blockout + detailed**; previews systems |
| **viewer** | none | none | prefab/model inspection |

### E. Renderer vs GPU backend (the split)
Today `SceneRenderer` fuses GPU ownership with orchestration. Target: **B2 "Renderer"**
(`src/engine/render/` — reconcile via signals, cull, LOD; writes components) over
**B1 "GPU backend"** (`src/engine/gpu/` — device/pipelines/passes; reads components).
The seam between them is, again, the render components in the one registry. **Name
chosen: Renderer (high) / GPU backend (low).**

## The data interfaces

1. **Components on the one registry, accessed via entt views/groups** — the universal seam. Sim systems, render systems, and apps communicate by reading/writing components on shared entities; each reads exactly the components it names through `registry.view<…>()` (or `group<…>` for hot paths). No snapshot copy, no facade layer anywhere in-process; structural changes are observed via entt signals.
2. **`MapData`** — the world-gen seam (immutable lattice; query face for sim, lattice face for B2).
3. **Language FFIs** — Rust seams for noiser (brains), `nav`, `assets`, `wesl`. Data-only, unchanged.

## Worked example — terrain, end to end (unified)

1. **World gen (C):** produces heightmap + biome as `MapData`. Procgen (mapview) or predefined/symbolic (game, ai_sandbox — deterministic).
2. **Sim (A):** systems read `MapData` **queries** (`HeightAt` for placement, `BiomesAt`/nav for pathing) and write game + `Transform` components on entities in the shared world. Never touch the terrain mesh.
3. **App (D):** creates the terrain **entity** — render components (indexed mesh + material + `Transform`) + a `TerrainLodComponent` carrying the heightfield/colors (from the `MapData` lattice). Also seeds sim/building entities. The app supplies data; makes no rendering decisions.
4. **Renderer (B2):** builds the terrain **cluster DAG** once (from the generic heightfield), then **each frame a terrain-LOD function** — a plain function over `registry.view<TerrainLodComponent, MeshDrawRangesComponent>()` — selects the cut from the camera and **updates the draw-range component**. It also culls and reads live transforms. (Your "terrain-graph ticks each frame and updates the LODs" — realized as an entt view + a Renderer-invoked function; no new subsystem.)
5. **GPU backend (B1):** uploads the mesh **once**, and each pass draws the selected index ranges. Knows nothing of DAGs, LOD, heightmaps, or biomes — only "draw these ranges of this mesh with this material and transform."

B2→B1 hand-off = the render components. B2 writes; B1 reads.

## Roadmap (sequencing — unify the registry first)

- **Phase 1 — Unify the world.** Change `badlands_game_lib`'s interface from the data-only C ABI to C++ systems over a shared `entt::registry&`; apps create one registry and run sim systems + rendering over it. Sim keeps its components; the POD-snapshot boundary (`game_state`/`game_buildings`) is replaced by direct component access. Verify each app behaves identically. *(Flag: this trades the C ABI's headless-sim isolation for one-world simplicity — sim systems remain unit-testable on a bare registry, which preserves most of that value.)*
- **Phase 2 — Remove the scene-graph sync.** Delete `SceneGraph` + `SyncToRegistry`; render entities persist in the registry; structural changes use entt signals to mark dirty; the Renderer iterates views; transforms live. game_view / model_viewer / ai_sandbox stop rebuilding every frame and render identically (a real perf win: no per-frame vertex re-upload).
- **Phase 3 — Terrain into the Renderer.** Move cluster-LOD (`terrain_clusters.*`, `SelectClusters`) into the engine Renderer with a **generic heightfield input** (no `MapData`); add `TerrainLodComponent` + the per-frame terrain-LOD function; a shared `MapData`→heightfield adapter; both apps render terrain as entities via the Renderer LOD. Delete `src/game/map/cluster_terrain.*`.
- **Phase 4 (later) — Physical B1/B2 split.** Reorganize `src/engine/rendering/` into `gpu/` (backend) + `render/` (Renderer); split `SceneRenderer` accordingly.

## Layer invariants (grep-able gates)

- Sim systems include **no** render headers (no mesh/material/camera/LOD types).
- The Renderer includes **no** game/sim or world-gen types (`MapData`, biome, building kinds).
- **No** `SelectClusters`/cluster-DAG outside the engine Renderer.
- **One** registry; **no** `SceneGraph`/`SyncToRegistry`; **no** per-frame registry rebuild or per-frame mesh re-upload for unchanged entities.
- Terrain renders through **one** Renderer path in every app.

## Open / to-confirm

- Subdir naming for the B1/B2 split (`gpu/` + `render/` proposed) — Phase 4.
- Whether any thin C facade is kept for the sim (for out-of-process tooling) or the C ABI is fully replaced by the C++ registry API.
- Test strategy on the unified world (deterministic map + headless sim tick + render screenshot as the cross-layer integration test).
