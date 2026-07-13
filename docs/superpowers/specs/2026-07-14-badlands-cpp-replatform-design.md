# Design: Re-platform badlands to a C++/Dawn engine (ported from sampo) + game layer + Rust feature-libs

Status: approved design (2026-07-14). Scope of this spec: the overall architecture + **Stage 1**
(platform + core engine port). Later stages get their own specs.

## Context

badlands is currently a **Rust-hosted** app (winit + wgpu) driving a **C++/EnTT** game sim behind a C
FFI. The v0.35 rendering work (terrain, biomes, ploppables, PBR materials) made a deeper truth clear:
as the world, geometry generation, camera, input handling, and scene construction all belong to the
game (C++), the Rust renderer becomes an FFI-wrapped island, and a large marshalling layer exists only
to let C++ drive a renderer that could simply *be* C++.

badlands began as a Rust/wgpu hand-port **of** sampo — a complete C++/Dawn engine with exactly the
data-driven material + scene-graph system we want. So the decision is to **re-platform badlands to a
C++/Dawn/SDL3-hosted app by porting sampo's engine, adding only a game layer on top, and keeping the
substantial Rust dependency-backed features as linked libraries.**

The v0.35 **content** (biomes + baked heights, rocks/trees/lake, buildings with roofs/towers) and the
**C++ world logic** (terrain grid, footprints, occupancy, nav, movement, brains) are preserved and
re-plumbed onto the new engine. The current working Rust build is retained as the behavioral/visual
reference until the port reaches parity.

## Architecture

Three layers:

1. **C++/Dawn/SDL3 host + engine (ported from sampo).** Window + main loop + input pump (SDL3); GPU
   context (Dawn + tint); the lower-level API (`GpuPipelineGenerator`, `shader_reflection`,
   `FrameContext`, `vertex_layout`); the data-driven material system
   (`Material`/`MeshRenderingMaterial`/`MaterialInstance`/factory/requirements + reflection-derived
   bind-group layouts + 1×1 defaults + instance cache); the scene layer (`SceneGraph`/`SceneNode`/
   `SceneAttachment` + `SceneRenderer` **forward-opaque** pass + `tonemapping`); GPU mip generation;
   camera; input handling.
2. **Game layer (badlands C++, the only net-new layer).** The existing EnTT world sim (`game/src/*`),
   procedural geometry generation (terrain/ploppable/primitive mesh builders), scene construction (build
   the scene graph from the world), and brains (noiser).
3. **Rust feature-libs (C ABI static libs, linked via Corrosion).** `noiser` (scripting VM), `nav`
   (cavalier_contours + parry2d + pathfinding), `wesl` (`.wesl`→WGSL for tint, sampo's `wesl-ffi`
   pattern), `assets` (`image` JPEG-decode + `gltf` parse → raw pixels/metadata; **no** mips — those are
   generated GPU-side).

### Ownership boundary
- **C++** owns: window, GPU, render loop, renderer, world logic, geometry generation, camera, input,
  scene construction, GPU mip generation.
- **Rust** owns: the four feature-libs above, each behind a narrow C ABI. `nav` keeps its existing
  `GamePathfinder` contract. This preserves the "clean FFI seam" property the project values.
- **Dropped:** the Rust host (`winit`), `wgpu`, `naga`, and `src/{app,gpu,scene,ui,main}`.

### Build system (cargo → CMake)
Top-level `CMakeLists.txt` cribbed from `../sampo/CMakeLists.txt`:
- **SDL3** via `FetchContent`.
- **Dawn** from source, pinned SHA (copy sampo's `FetchDawnSource` handling + the `webgpu` interface
  target); C++23; Metal backend on macOS.
- **Corrosion** (`FetchContent`) + `corrosion_import_crate(MANIFEST_PATH <crate>/Cargo.toml)` per Rust
  crate, with `corrosion_set_env_vars(<crate> "RUSTFLAGS=-Cpanic=unwind")`; C++ targets link the crate
  name (e.g. `wesl_ffi`, `nav_ffi`) — exactly sampo's mechanism.
- Reuse the existing C++ third-party (glm, EnTT, spdlog) already vendored under `game/`/`third_party/`.

## Decomposition (each stage: spec → plan → build)
1. **Platform + core engine port** — *this spec*.
2. **Game-layer wiring** — C++ builds the scene graph from the world; camera + world input in C++; the
   `nav` Rust-lib reconnected.
3. **Content parity** — port the geometry generators (terrain/ploppable/primitives) to C++; the full
   v0.35 scene renders (biomes, heights, rocks/trees/lake, buildings + roofs/towers), C++-driven,
   matching the current Rust screenshot.
4. **UI** — port sidebar/panels (Rust `fontdue` → ImGui or a C++ UI) + world-vs-UI input routing.
5. **Post-fx (optional, later)** — deferred G-buffer, shadows, GTAO, bloom, water, skybox: modular
   `ProcessingGraph` nodes ported as desired.

---

## Stage 1 — Platform + core engine port (forward-first)

**Goal.** badlands is a C++/Dawn/SDL3 app that renders a hardcoded lit, textured, **mipmapped** test
scene through the **ported** engine (material system + scene graph + forward-opaque pass + tonemap),
with the Rust feature-libs linked and callable across the C ABI.

**Build.**
- New top-level `CMakeLists.txt` (as above): SDL3 + Dawn(from-source, pinned) + Corrosion; C++23; Metal.
- Corrosion-import the Rust crates as static libs: `noiser` (existing), plus new thin wrapper crates
  `wesl` (wrap the `wesl` crate → `.wesl`→WGSL C ABI, per sampo's `wesl-ffi`) and `assets` (wrap
  `image` + `gltf` → decode/parse C ABI), and `nav` (extract the existing `src/nav.rs` into a crate
  behind its current `GamePathfinder` C ABI).
- The existing C++ game lib (`game/src/*`) compiles under CMake (not yet wired to rendering).

**Port from sampo** (copy + adapt namespaces/includes; bring along the support types each depends on —
`GeometryType`, resource manager, EnTT components):
- `core/camera.hpp` (Camera + `UniformData`, reversed-Z) + math glue.
- `rendering/context/{gpu_context, frame_context}`, `rendering/shader/{gpu_pipeline_generator,
  shader_reflection}`, `rendering/vertex_layout.*`.
- `rendering/material/*` — the full material system.
- `scene/{scene_graph, scene_node, scene_attachment}`, `rendering/components/*` (mesh, transform,
  material-factory), `rendering/scene_renderer` (**forward-opaque + tonemap subset only**),
  `rendering/passes/render_textured_mesh`, `geometry/textured_mesh_builders`.
- Shaders: `common/frame.wesl`, `material/{normalmapped,textured_mesh}.wesl`, `passes/tonemapping.wesl`.
- GPU mip generation: `image_processing/tasks/mipmap_generation_task` + `compute/mip_generator*.wesl`.

**Data flow proven by Stage 1.** WESL (Rust `wesl` lib) → WGSL → tint/Dawn pipeline + C++ reflection →
material instance (bind group) → scene-graph node (ported mesh builder) → forward pass → tonemap →
SDL3 window. A JPEG decoded by the Rust `assets` lib → Dawn texture → **GPU-generated mips** → sampled.

**Milestone.** The app opens an SDL3 window and renders a lit, textured, mipmapped **sphere** (sampo's
`GenerateSphereTexturedMesh`, which matches the glTF packs' UV-sphere) through the ported material +
scene-graph + forward pipeline.

**Verification.**
- CMake configures + builds; Dawn + SDL3 + the four Rust static libs (noiser/wesl/assets/nav) link.
- Run the app: the test scene renders (visual; capture a screenshot if a headless/offscreen path is
  wired, otherwise a windowed run).
- C-ABI smoke checks: `wesl` compiles one shader to WGSL; `assets` decodes a known JPEG to expected
  dimensions; `nav` links and answers a trivial path query.

## What's reused / dropped
- **Reused (C++, unchanged this stage):** the game sim `game/src/*` (world, placement, movement,
  terrain grid, ploppables, heroes/brains) — compiled under CMake, wired to rendering in Stage 2.
- **Reused (Rust, re-exposed as static libs via Corrosion):** `src/nav.rs`, `noiser`, plus new thin
  `wesl` and `assets` wrapper crates.
- **Dropped:** `src/{app,gpu,scene,ui,main}`, the `wgpu`/`naga`/`winit` deps, and the v0.35
  game-specific render FFI (`game_terrain`/`game_ploppables`/`game_ploppable_boundary`/…).

## Risks & mitigations
- **Dawn build weight** — copy sampo's working from-source CMake (pinned SHA); it's a solved setup.
- **Rust static-lib ↔ CMake linking** — Corrosion, exactly as sampo does it; noiser already crosses the
  Rust↔C++ boundary today, so the toolchain is proven.
- **Engine drift from sampo** — we own the port; divergence is accepted (the "own it" choice).
- **Porting support types** — sampo's engine assumes its `GeometryType`, resource manager, and EnTT
  conventions; port those support pieces alongside the core, not just the headline classes.
- **Large effort** — staged; the working Rust build remains the behavioral/visual reference until
  Stage 3 reaches parity.
