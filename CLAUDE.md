# CLAUDE.md

Guidance for Claude Code (claude.ai/code) in this repository.

badlands is a Majesty-style town-and-heroes prototype: a C++/Dawn(WebGPU)/SDL3 engine
driving a C++/EnTT simulation, with per-entity AI brains. Build and run instructions
live in `README.md`. Design/plan notes live under `docs/` (`docs/superpowers/specs/`,
`docs/superpowers/plans/`, `docs/design/`, `docs/brainshitting/`).

**Directory-local rules live in nested CLAUDE.md files.** Read the one for the area you
are touching:

| Area | File |
|---|---|
| EnTT world sim — clocks, commands, brains, combat | `game/CLAUDE.md` |
| Renderer, GPU, materials, scene graph | `src/engine/CLAUDE.md` |
| The native RHI (Metal/DX12) — **foundational, stricter rules** | `src/engine/rhi/CLAUDE.md` |
| Render/scene layer — geometry, LOD chains, impostors | `src/game/CLAUDE.md` |
| WESL/WGSL shaders | `shaders/CLAUDE.md` |
| Rust feature-libs and the C ABIs | `src/crates/CLAUDE.md` |
| Apps, `AppView`s, AI-sandbox modes, CLI flags | `src/executables/CLAUDE.md` |
| Map data contract, patch extraction, rivers | `src/mapgen/CLAUDE.md` |
| Nim→WASM brains | `scripts/brains/CLAUDE.md` |
| Shapeshifter, the SDF editor — Swift/SwiftUI shell, two build systems | `editors/shapeshifter/CLAUDE.md` |

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
- **NEVER ADD ANYTHING TO A UI WITHOUT EXPLICIT APPROVAL — game UI AND debug ImGui alike.** No window, panel, tab, text line, checkbox, slider, plot or menu entry unless I asked for that specific element. This binds even when the element would feed a feature I *did* approve: the feature is approved, its UI is not.
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

### Once a plan is approved, RUN IT
The gates above are for *before* approval. After it, the default is to keep going
to the end of the plan.

- **Every plan marks its CORE DELIVERABLES and CORE ASSUMPTIONS explicitly**, in their own section. Everything not on those lists is a detail.
- **Only a core item stops execution.** If a discovery invalidates a core deliverable or a core assumption, stop and ask me to recalibrate.
- **Details may be overridden without asking**, provided the change is driven by a discovery that invalidated the original and is reported transparently at the end. Choosing a different enum default, a different helper's home, an extra shared bounds check — decide it, do it, tell me afterwards.
- **Do NOT end a turn to report progress.** Interesting findings, tests going green, a defect caught mid-task: these are turn *content*, not turn boundaries. Report them in one pass when the work is done.
- **Report deviations as a list at the end**, each with what was found and why it forced the change. Silent deviation is the thing being prevented — not deviation itself.

## Layer map — the boundary is deliberate
- **`src/engine/`** — engine ported from the sibling project `sampo` (`/Users/jakub/repos/sampo`): rendering, GPU/pipeline/reflection/frame infra, material system, scene graph, `Camera`, SDL app shell. **No game logic or game types.**
- **`src/game/`** — C++ game *render/scene* layer: geometry generation, map data, scene composition, UI logic, picking. It is not the simulation.
- **`game/`** — the EnTT world **simulation** (placement, movement, brains, combat, needs), built as `badlands_game_lib` and called through a C ABI. The `src/game/` vs `game/` split is a real trap: sim code never lives under `src/`.
- **`src/core/`** — generic shared C++ (math glue, `GeometryType`, small utils).
- **`src/crates/`** — Rust feature-libs behind narrow C ABIs, linked via Corrosion.
- **`src/mapgen/`** — the map DATA CONTRACT (`PatchRequest` → `PatchData`), its providers, on-disk artifact I/O, and the river chain. **Not a pipeline stage** — all three procgen stages sit on it. There is no in-repo generator any more; the name is historical.
- **`src/mapview/`** — terrain/biome presentation for `badlands_mapview`.
- **Procgen is three stages with ONE interface between them:** `coarse-hydraulic-erosion-sim` (`tools/protogen/`, standalone, cached on disk) → `detailed-patch-extraction` (`PatchRequest` → `PatchData`) → `map-detailing` (a sink: the existing mapview/game render path). The boundary is enforced by CMake targets, not convention — stage 3 links the contract and not the providers. See `docs/superpowers/specs/2026-08-02-procgen-stage-split-design.md`.
- **`src/executables/`** — the apps; each builds its scene from the world in its own `AppView`.
- **`editors/`** — authoring tools, outside the `src/` layer rules. Only `editors/shapeshifter/` today: the SDF editor, moved in from its own repo. It is the one place with a Swift/SwiftUI app and a second build system (XcodeGen), and the one place that still drives Metal through metal-cpp rather than `src/engine/rhi`. Read `editors/shapeshifter/CLAUDE.md` before touching it.

Ownership: **C++ owns** window, GPU, render loop, renderer, world, geometry, camera,
input, scene construction. **Rust owns** the feature-libs.

## Commands (prefer these over ad-hoc pipelines — compact output)
- `scripts/build.sh [target …]` — configure (first run) + build; errors only + `BUILD OK/FAILED`.
- `scripts/test.sh` — full ctest summary. `scripts/test.sh <regex>` — one suite via `ctest -R`,
  failures+totals only. `scripts/test.sh <test-binary> "<catch2-filter>"` — direct filtered run
  (e.g. `scripts/test.sh badlands_game_tests "[combat]"`).
- `scripts/screenshot.sh <app> <out.png> [args…]` — SIGALRM-bounded headless screenshot.
- `scripts/gitstate.sh` — branch + HEAD + short status + commits ahead of origin/main.
- `scripts/noiser_guard.sh [BASE]` — assert `third_party/noiser` isn't staged (nor in `BASE..HEAD`).
- ctest is canonical for C++ tests (~35 targets); running a test binary bare can fail on missing env.
- **ctest does not cover the Rust crates.** Test those with `cargo test --manifest-path src/crates/<crate>/Cargo.toml --lib` — the `--lib` is mandatory, since bare `cargo test` here runs only the empty doctest target (details in `src/crates/CLAUDE.md`).

## Rules you can break before you open the directory
- **Dawn is pinned** (SHA in `cmake/FetchDawn.cmake`). Do not bump it without approval.
- **Slang is pinned too** (version + sha256 in `scripts/fetch_slang.sh`), and REQUIRED — configure fails without it. Same rule: do not bump without approval.
- **Binary assets are git LFS** (`.gitattributes`: `*.bin/*.exr/*.jpg/*.jpeg/*.png/*.ttf/*.wasm`); a plain `git add` on one stores an LFS pointer, so stage asset paths deliberately.
- **Four time bases, and they never mix:** `real_*` (app shell only), `anim_*` (views only), `*_ticks` (int64, 1 tick = 1/120 s — everything under `game/`), narrative `*_hours` (derived). Full contract in `game/CLAUDE.md`; nothing under `game/` may see a real or presentation `dt`.
- **FFI is data-only.** Cross-language seams are narrow C ABIs with no game concepts leaking into the Rust libs.
- `build/`, `target/`, `.claude/`, and `.superpowers/` are gitignored.
