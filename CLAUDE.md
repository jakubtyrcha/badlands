# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working agreement
- **Concise. Bullet points. No fluff.**
- **Frontload interface design.** Clarify details and assumptions with the user *before* building. Make autonomous decisions only for implementation details — never for interfaces or architecture.
- **The rendering/engine interface is general and stable.** Keep it game-agnostic (no game types in `src/engine/` or `src/core/`). ALWAYS get user approval before changing the rendering/engine interface.
- **UI is two separate surfaces:** game UI (in-world pane) vs debug UI (Dear ImGui). Do not conflate them.
- **Don't build debug controls that weren't asked for.** No ImGui panels/toggles/sliders, no env-var hooks, no "demo mode" switches unless the user asks. Ship the feature with fixed constants; do NOT add a config struct, a style object, or plumbing whose only purpose is to feed a control that doesn't exist. If tuning genuinely needs a knob, ask first.
- Co-design one decision at a time. For features, use superpowers brainstorming → spec → plan → subagent-driven-development.
- **Simplest thing for data presentation.** To show data/results, reach for the simplest option: write files and point to the paths, print a table. Build elaborate tools/visualizations/galleries only when explicitly asked.
- **Work on `main` (or a normal branch) — avoid git worktrees here.** Dawn/CMake builds are heavy; worktrees multiply artifacts (`build/`, `target/`, Dawn cache).

### Think before coding
Don't assume. Don't hide confusion. Surface tradeoffs. Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## Repository state
badlands runs on **C++/Dawn/SDL3**, with the engine ported from the sibling project `sampo` (`/Users/jakub/repos/sampo`) and Rust feature-libs linked via Corrosion. The migration off the old Rust/winit/wgpu app is **largely complete** and all work lives on `main`; built with CMake.
- **Not yet ported:** terrain & biomes. `badlands_game` still renders a static demo town; `badlands_ai_sandbox` is the live view of the sim (ticking world, blockout capsules, inspector panel).
- **Hero brain is Nim→WASM**, run in a wasmtime host (`src/crates/brainhost`) behind the wire contract `game/src/brain_abi.h`; sources in `scripts/brains/nim/`, built to LFS-committed `.wasm` under `assets/brains/`. The noiser brain path stays compiled and dormant behind `BrainDesc` (mapgen/texgen still use noiser).
- **Game systems are event-sourced** (`game/src/command.h`): every mutation — player action and AI decision alike — is a `Command` applied at one point and appended to `command_log`. `state = f(initial config, command log, N ticks)`, enforced by `game/tests/determinism_tests.cpp` (run-twice + replay-the-log). New mechanic = new `CommandKind` + handler; never a direct registry write from a brain.
- Design/plan notes live under `docs/` (`docs/superpowers/specs/`, `docs/superpowers/plans/`, `docs/brainshitting/`).

## Build & run
Run from the repo root (`shaders/` + `assets/` resolve relative to cwd).
```sh
cmake -S . -B build -G Ninja                          # configure (first Dawn-from-source build is long, then cached)
cmake --build build                                   # builds the apps + Rust staticlibs
./build/badlands_game                                 # run (opens an SDL3 window); also: badlands_viewer, badlands_ai_sandbox
./build/badlands_game --screenshot out.png            # headless: render one frame to PNG (offscreen readback)
./build/badlands_game --record frames/                # headless: render a frame sequence into a dir
perl -e 'alarm 30; exec @ARGV' ./build/badlands_game  # SIGALRM-bounded headless smoke run
```
`badlands_mapview` is the map tool: it generates a map procedurally (bedrock
field → quantile-cut biomes) and renders it as biome-colored terrain.
`--preview-image-only` instead dumps the debug rasters (bedrock/biome/heightmap/hillshade
PNGs) to `--out` and exits (pure CPU, no window).
```sh
./build/badlands_mapview --seed 2 --resolution 500x500 --size 500x500   # view it
./build/badlands_mapview --preview-image-only --out mapgen_out          # dump PNGs
```
Rust feature-lib tests — **use `--lib`** (bare `cargo test` here prints only the empty doctest target):
```sh
cargo test --manifest-path src/crates/wesl/Cargo.toml      --lib
cargo test --manifest-path src/crates/assets/Cargo.toml    --lib
cargo test --manifest-path src/crates/nav/Cargo.toml       --lib
cargo test --manifest-path src/crates/ui/Cargo.toml        --lib
cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib
#   single test: append its name, e.g. ... --lib write_png_roundtrip
```
- Prereqs: `git-lfs` + initialized submodules (`noiser`, `catch2`, `entt`, `glm`, `spdlog`). See README for clone/LFS setup.
- `scripts/build_brains.sh` rebuilds the committed brain `.wasm` artifacts (needs Nim + a pinned wasi-sdk, auto-fetched); the artifacts are LFS-committed, so normal builds don't need the toolchain.
- Dawn is pinned (SHA in `cmake/FetchDawn.cmake`). Do not bump it without approval.
- C++ tests are Catch2 targets (`badlands_game_tests`, `badlands_geometry_tests`): run `ctest --test-dir build`, or the binaries directly.

## Architecture — the layer boundary is deliberate
- **`src/engine/`** — engine ported from sampo (`sampo::` → `badlands::`): rendering, GPU/pipeline/reflection/frame infra, data-driven material system, scene graph + scene renderer (forward-opaque + tonemap), GPU mip generation, `Camera`. **No game logic or game types.**
- **`src/game/`** — C++ game render/scene layer: geometry generation, scene construction, per-app `AppView`s (`GameView`, etc.), camera + input *handling*, UI *logic*. The EnTT world sim (placement/movement/brains/combat) lives in **`game/`**, built as `badlands_game_lib` and called by the apps through a C ABI.
- **`src/core/`** — generic shared C++ (math glue, `GeometryType`, small utils).
- **`src/crates/`** — Rust feature-libs behind narrow C ABIs, linked via Corrosion: `wesl` (`.wesl`→WGSL + reflection), `assets` (JPEG decode + glTF parse + PNG write), `nav` (`GamePathfinder` pathfinding), `ui` (game-UI layout via `panes` + text via `fontdue`), `brainhost` (wasmtime brain-wasm host behind a C ABI). `noiser` (scripting) lives under `third_party/`, wired in via `crates/noiser-bundle`.

Ownership: **C++ owns** window, GPU, render loop, renderer, world, geometry, camera, input, scene construction, GPU mips. **Rust owns** the feature-libs. Each app builds its scene from the world in its own `AppView`.

Data flow: WESL (Rust) → WGSL → Dawn pipeline + reflection → material instance (bind group) → scene-graph node → forward pass → tonemap → SDL3. JPEG (Rust `assets`) → Dawn texture → GPU mips → sampled.

## Non-obvious conventions (read the cited code before touching these)
- **Shader reflection is naga-in-Rust, not tint.** `shader_reflection.cpp` / `gpu_pipeline_generator.cpp` call the `wesl` crate's `wgsl_reflect*` (naga). Pipelines use **explicit reflection-derived** bind-group layouts (not Dawn AUTO); build bind groups via `CreateBindGroup(device, pipeline, group, entries)`.
- **Reversed-Z end to end:** depth clears to `0.0` (far); opaque depth-compare `GreaterEqual` (`Less` only for the shadow pass); `GLM_FORCE_DEPTH_ZERO_TO_ONE` is set project-wide; `Camera::GetProj` maps near→1, far→0. `static_assert(sizeof(UniformData)==592)` must hold and match `shaders/common/frame.wesl`.
- **The per-object uniform buffer is the `group==1` UBO, not `uniform_buffers[0]`** — reflection returns *all* UBOs including the group-0 `frame` UBO, whose emission order varies by shader.
- **Material textures resolve by `param_name == slot_name`** (e.g. `textured_mesh`'s albedo slot is `"mesh_texture"`). `InstanceParams.texture_overrides` carry their own sampler; the factory's default sampler uses `mipmapFilter=Nearest`, so supply a trilinear+aniso sampler when you want the mip chain used.
- **Corrosion crate quirks:** each `src/crates/*/Cargo.toml` needs an empty `[workspace]` table (so cargo doesn't walk up to the root workspace, whose `noiser-bundle` member isn't checked out here); crate profiles set `panic="abort"` and Corrosion overrides to `-Cpanic=unwind` at link so the extern-"C" `catch_unwind` thunks actually catch. The `wesl` crate's Cargo *target* is named `wesl_ffi` to avoid colliding with the `wesl` dependency.
- **FFI is data-only and mockable.** Cross-language seams are contracts (narrow C ABIs, tested across the boundary). Keep them low-level — no game concepts leak into the Rust libs.
- **Binary assets are git LFS** (`*.bin/*.jpg/*.jpeg/*.png/*.ttf`); a plain `git add` on one stores an LFS pointer, so stage asset paths deliberately. `build/`, `target/`, `.claude/`, and `.superpowers/` are gitignored.
