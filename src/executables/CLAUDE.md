# src/executables/ — the apps

Each app owns an `AppView` and builds its scene from the world itself; the app shell
(`engine/app/sdl_viewer_app.hpp`) owns the window, GPU and loop.

| App | What it is |
|---|---|
| `badlands_game` | the game: symbolic-map terrain + biome PBR packs, town, heroes |
| `badlands_ai_sandbox` | the AI harness, driven by a `--mode` |
| `badlands_viewer` | model/LOD viewer |
| `badlands_mapview` | the map tool (see `src/mapgen/CLAUDE.md`) |
| `badlands_rhi_lab` | the RHI/Slang/visibility-buffer MVP — PNG by default, `--window` for a live camera |

## `badlands_rhi_lab` is not an `AppView`
It builds on `badlands_rhi` + `badlands_slang`, not `badlands_engine`, and owns its
own SDL loop. That is deliberate, not temporary: `SdlViewerApp` is Dawn all the way
through (`RenderContext` hands views a `wgpu::Device`), so the RHI-era app shell is a
separate decision, taken when the render graph gives an `RhiView` something to mean.
Skipped by the build when the Slang SDK is absent (`tools/slang_probe/README.md`).
```sh
./build/badlands_rhi_lab --out /tmp/lab.png              # materials + lighting
./build/badlands_rhi_lab --out /tmp/vis.png --debug-vis  # the visibility buffer
./build/badlands_rhi_lab --window                        # live: WASD/QE, right-drag to look
```
`ctest -L display` runs the windowed resize test; it is out of the default suite
because it needs a real display.

## Common flags
Run from the repo root — `shaders/` and `assets/` resolve relative to cwd.
```sh
./build/badlands_game --screenshot out.png   # headless: render one frame to PNG
./build/badlands_game --record frames/       # headless: render a frame sequence
scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 5
```
- `badlands_viewer --lod`: 0 = original cards (now deferred alpha-cutout, not forward), 1–3 = voxel LODs L0..L2, **4 = Impostor** (the baked octahedral billboard, which took over the slot voxel L3 used to hold), 5 = Multi (instanced field). Multi drives the same chain the game does, impostor included: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`), capped by the compile-time `kMaxLods` (8). Voxel L3 is still BUILT (the field selects it between 70 and 130 preview metres) but is no longer reachable from the viewer's manual switch.
- `badlands_game` reads `USE_BLOCKOUT_MODE` (any non-empty value) to render greybox proxies instead of detailed PBR materials.

## The AI sandbox is a puppet-master layer, and it is one-way
- **A MODE says what world to build and who is in it**; the app then ticks and draws it. Modes today: `duel` (default), `sneak`, `teleport`, selected with `--mode` (`--seed N` for duel).
- **Nothing in `game/` or `badlands_game_lib` knows a mode exists.** Every system under observation behaves exactly as it does in the real game.
- **Testability never outranks layering.** If a mode needs a hook inside the sim, that is the wrong design — build the setup from Commands the game already has.
- Modes are view-free and are tested headless by `badlands_ai_sandbox_tests`.
