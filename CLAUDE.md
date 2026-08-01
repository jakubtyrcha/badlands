# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## System Rules
* Use **bullet points**.
* Write a maximum of **two sentences** per bullet point.
* Write **one instruction** per sentence. 
* State the **condition first**. Example: "If the file exists, delete it."
* If I do not ask for details, write only the core answer.
* Make **one architectural decision** at a time.
* If we develop a new feature, execute the sequence below:
  1. **Ideas:** List unconstrained concepts.
  2. **Spec:** Write the technical specification. Ask for missing requirements.
     * Stop and wait for my approval.
  3. **Plan:** Write the step-by-step execution tasks.
  4. **Code:** Write the code for sub-agents. 
     * If the implementation must deviate from the spec, stop. Ask me to recalibrate.

## Working agreement
- **Frontload interface design.** Clarify details and assumptions with the user *before* building. Make autonomous decisions only for implementation details — never for interfaces or architecture.
- **The rendering/engine interface is general and stable.** Keep it game-agnostic (no game types in `src/engine/` or `src/core/`). ALWAYS get user approval before changing the rendering/engine interface.
- **UI is two separate surfaces:** game UI (in-world pane) vs debug UI (Dear ImGui). Do not conflate them.
- **Don't build debug controls that weren't asked for.** No ImGui panels/toggles/sliders, no env-var hooks, no "demo mode" switches unless the user asks. Ship the feature with fixed constants; do NOT add a config struct, a style object, or plumbing whose only purpose is to feed a control that doesn't exist. If tuning genuinely needs a knob, ask first.
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
- **Not yet ported:** terrain & biomes. `badlands_game` still renders a static demo town.
- **`badlands_ai_sandbox` is the AI harness, driven by a MODE** (`--mode duel` today): a mode says what world to build and who is in it, the app ticks and draws it. The layer is one-way — nothing in `game/` or `badlands_game_lib` knows a mode exists, so every system under observation behaves exactly as it does in the real game. Modes live in `src/executables/ai_sandbox/` and are view-free (tested headless by `badlands_ai_sandbox_tests`).
- **Hero brain is Nim→WASM, the only hero brain** — run in a wasmtime host (`src/crates/brainhost`) behind the wire contract `game/src/brain_abi.h`; sources in `scripts/brains/nim/`, built to LFS-committed `.wasm` under `assets/brains/`. No mock/C++ hero decision layer remains; a world with no wasm bytes loaded simply idles heroes.
- **Combat is brain-driven end-to-end through the intention/action gateway** — wasm heroes and engine-code monster brains (rats, goblins) both fight by adopting an `Attack` intention (engagement) and enqueueing `BL_ACT_ATTACK` actions (swings) through the same `apply_intention`/`resolve_action` seams (`game/src/intention.h`); there is no separate host-level combat path. Defense is passive-only (`resolve_attack`'s defender gates) — a brainless hero issues no intentions/actions.
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
`badlands_mapview` is the map tool: it generates a map procedurally (bedrock field →
quantile-cut biomes → stream-power erosion + lakes) and renders it as cluster-LOD
terrain wearing one PBR material per biome (blended from a biome splat texture),
with still, murky lake water. `--preview-image-only` instead dumps the debug rasters
(bedrock/biome/heightmap/hillshade/flow/water/sediment PNGs plus a numbered
per-stage + per-N-iterations film strip) to `--out` and exits (pure CPU, no
window).
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
- Prereqs: `git-lfs` + initialized submodules (`catch2`, `entt`, `glm`, `spdlog`, `meshoptimizer`). See README for clone/LFS setup.
- `scripts/build_brains.sh` rebuilds the committed brain `.wasm` artifacts (needs Nim + a pinned wasi-sdk, auto-fetched); the artifacts are LFS-committed, so normal builds don't need the toolchain.
- Dawn is pinned (SHA in `cmake/FetchDawn.cmake`). Do not bump it without approval.
- C++ tests are Catch2 targets (`badlands_game_tests`, `badlands_geometry_tests`): run `ctest --test-dir build`, or the binaries directly.

## Commands (prefer these over ad-hoc pipelines — compact output)
- `scripts/build.sh [target …]` — configure (first run) + build; errors only + `BUILD OK/FAILED`.
- `scripts/test.sh` — full ctest summary. `scripts/test.sh <regex>` — one suite via `ctest -R`,
  failures+totals only. `scripts/test.sh <test-binary> "<catch2-filter>"` — direct filtered run
  (e.g. `scripts/test.sh badlands_game_tests "[combat]"`).
- `scripts/screenshot.sh <app> <out.png> [args…]` — SIGALRM-bounded headless screenshot
  (e.g. `scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 5`).
  `--lod`: 0=Original cards, 1-4=voxel LODs L0..L3, 5=Multi (instanced field).
  L3 is the coarse tail (~230-680 tris/tree). Multi drives the same L0..L3
  chain: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`),
  capped by the compile-time `kMaxLods` (8).
- `scripts/gitstate.sh` — branch + HEAD + short status + commits ahead of origin/main.
- `scripts/noiser_guard.sh [BASE]` — assert `third_party/noiser` isn't staged (nor in `BASE..HEAD`).

## Time convention — FOUR clocks, and they never mix
There are four time bases. Every duration, rate and timestamp belongs to exactly one, and the name says which.

| Base | What it is | Name / unit | Who may read it |
|---|---|---|---|
| **Real** | wall clock; ignores pause and speed | `real_dt`, `real_*`, float seconds | the app shell only (SDL loop, SimClock input) |
| **Presentation** | real × playback speed; stops when paused | `anim_*`, float seconds | views only — animation, VFX, tracers, camera easing |
| **Sim** | the deterministic tick clock | `*_ticks` (int64), **1 tick = 1/120 s** | everything under `game/`. THE gameplay clock |
| **Narrative** | day/night, derived from sim time | `*_hours`, `time_of_day`, `*_days` | the day cycle, and needs |

- **Authored data is in SIM SECONDS.** A `3` in `skills.json` is 3 sim seconds; at 1× playback that happens to be 3 real seconds, and that equality is a property of the speed setting, not the definition.
- **A TICK is the atom of sim time, and a tick is 1/120 s** (`kTicksPerSecond = 120`). 120 was chosen because it divides every usual step rate — 30, 60, 120 — so a sim step is always a whole number of ticks and no clock can drift against another.
- **Sim time is NOT milliseconds.** 1000 does not divide by 30, so a millisecond clock at 30 Hz truncates (33 ms/step → 30 steps = 990 ms, a 1% drift against the clock that decides how many steps to run). Ticks remove the rounding rather than documenting it.
- **The sim STEP is separate from the tick.** The step rate is how often the world advances (30 Hz today = `kTicksPerStep` 4); the tick is what time is *measured* in. Changing the step rate must never change what a duration means.
- Authored seconds convert once, at the boundary: `ticks = round(seconds × 120)`. Milliseconds survive only where a wire or a display asks for them.
- **Nothing under `game/` may see real or presentation time.** If a sim system takes a `dt`, that is the bug: it makes the result depend on a number that is not in the command log, which the determinism contract forbids.
- **Narrative time is DERIVED and is never a gameplay duration.** A cooldown is never "half an in-game hour". One deliberate exception, and it says so in its signature: `reserve_rate_per_tick(hours, millis_per_day)` — a need is *about* the day, so it is authored against it.
- **Speed changes how fast ticks are issued, never what a tick means.** `SimClock` decides how many ticks to run; the length of one is a compile-time constant.
- When talking to a human, real-world units are fine ("a 3-second stun"). In code and data they mean sim time, always.

## Architecture — the layer boundary is deliberate
- **`src/engine/`** — engine ported from sampo (`sampo::` → `badlands::`): rendering, GPU/pipeline/reflection/frame infra, data-driven material system, scene graph + scene renderer (forward-opaque + tonemap), GPU mip generation, `Camera`. **No game logic or game types.**
- **`src/game/`** — C++ game render/scene layer: geometry generation, scene construction, per-app `AppView`s (`GameView`, etc.), camera + input *handling*, UI *logic*. The EnTT world sim (placement/movement/brains/combat) lives in **`game/`**, built as `badlands_game_lib` and called by the apps through a C ABI.
- **`src/core/`** — generic shared C++ (math glue, `GeometryType`, small utils).
- **`src/crates/`** — Rust feature-libs behind narrow C ABIs, linked via Corrosion: `wesl` (`.wesl`→WGSL + reflection), `assets` (JPEG decode + glTF parse + PNG write), `nav` (`GamePathfinder` pathfinding), `ui` (game-UI layout via `panes` + text via `fontdue`), `brainhost` (wasmtime brain-wasm host behind a C ABI).

Ownership: **C++ owns** window, GPU, render loop, renderer, world, geometry, camera, input, scene construction, GPU mips. **Rust owns** the feature-libs. Each app builds its scene from the world in its own `AppView`.

Data flow: WESL (Rust) → WGSL → Dawn pipeline + reflection → material instance (bind group) → scene-graph node → forward pass → tonemap → SDL3. JPEG (Rust `assets`) → Dawn texture → GPU mips → sampled.

## Non-obvious conventions (read the cited code before touching these)
- **Shader reflection is naga-in-Rust, not tint.** `shader_reflection.cpp` / `gpu_pipeline_generator.cpp` call the `wesl` crate's `wgsl_reflect*` (naga). Pipelines use **explicit reflection-derived** bind-group layouts (not Dawn AUTO); build bind groups via `CreateBindGroup(device, pipeline, group, entries)`.
- **Reversed-Z end to end:** depth clears to `0.0` (far); opaque depth-compare `GreaterEqual` (`Less` only for the shadow pass); `GLM_FORCE_DEPTH_ZERO_TO_ONE` is set project-wide; `Camera::GetProj` maps near→1, far→0. `static_assert(sizeof(UniformData)==592)` must hold and match `shaders/common/frame.wesl`.
- **The per-object uniform buffer is the `group==1` UBO, not `uniform_buffers[0]`** — reflection returns *all* UBOs including the group-0 `frame` UBO, whose emission order varies by shader.
- **Material textures resolve by `param_name == slot_name`** (e.g. `textured_mesh`'s albedo slot is `"mesh_texture"`). `InstanceParams.texture_overrides` carry their own sampler; the factory's default sampler uses `mipmapFilter=Nearest`, so supply a trilinear+aniso sampler when you want the mip chain used.
- **Corrosion crate quirks:** each `src/crates/*/Cargo.toml` needs an empty `[workspace]` table (so cargo doesn't walk up the tree looking for a parent workspace); crate profiles set `panic="abort"` and Corrosion overrides to `-Cpanic=unwind` at link so the extern-"C" `catch_unwind` thunks actually catch. The `wesl` crate's Cargo *target* is named `wesl_ffi` to avoid colliding with the `wesl` dependency.
- **FFI is data-only and mockable.** Cross-language seams are contracts (narrow C ABIs, tested across the boundary). Keep them low-level — no game concepts leak into the Rust libs.
- **Terrain layer blending is height-lerped, and displacement rides in ARM alpha.** `shaders/common/terrain_layers.wesl` is shared by `terrain_blend` (weights per VERTEX) and `terrain_cluster` (weights from a biome SPLAT texture sampled in world XZ, so they survive LOD decimation); `LoadTerrainArrays` folds each pack's `disp` red into its `arm` alpha, so height blending costs no extra array, binding, or fetch.
- **Water is a Beer-Lambert medium, not a tint.** `water.wesl` takes a per-channel extinction in 1/m plus a scattering albedo; the shore fade and the depth hue shift both fall out of transmittance, so there is no coast-width or absorption knob and the surface outputs alpha 1. Extinction is always stated as a visibility depth and converted (`sigma = 3 / d_vis`). The `still` shader feature is standing water (no waves, flat normal) — it only ever reaches the forward-transparent variant, since `kForwardTransparent` registers no shadow pass.
- **`SceneGraph::SyncToRegistry` starts with `registry.clear()`.** An app that creates entities directly in its registry (mapview: the cluster terrain, the lake water) cannot also drive a SceneGraph over that same registry — the sync would wipe them every frame.
- **Binary assets are git LFS** (`*.bin/*.jpg/*.jpeg/*.png/*.ttf`); a plain `git add` on one stores an LFS pointer, so stage asset paths deliberately. `build/`, `target/`, `.claude/`, and `.superpowers/` are gitignored.
