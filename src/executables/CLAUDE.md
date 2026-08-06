# src/executables/ — the apps

Each app owns an `AppView` and builds its scene from the world itself; the app shell
(`engine/app/sdl_viewer_app.hpp`) owns the window, GPU and loop.

| App | What it is |
|---|---|
| `badlands_game` | the game: symbolic-map terrain + biome PBR packs, town, heroes |
| `badlands_ai_sandbox` | the AI harness, driven by a `--mode` |
| `badlands_viewer` | model/LOD viewer, plus the character/skeleton viewer (`--character`) |
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
- **`--lod` means different things for a tree and for a prop, and a prop's chain length is not a constant.**
  - **Trees:** 0 = original cards (deferred alpha-cutout, not forward), 1–3 = voxel LODs L0..L2, **4 = Impostor** (the baked octahedral billboard, which took over the slot voxel L3 used to hold), 5 = Multi (instanced field). Voxel L3 is still BUILT (the field selects it between 70 and 130 preview metres) but is no longer reachable from the manual switch.
  - **Props:** 0 = source mesh, 1..N-1 = triangle LODs, N = Impostor, N+1 = Multi — where **N is derived per model** from its size and triangle count (`src/game/visual/lod_screen_space.hpp`), so boulder_01 gets 4 mesh levels and the war hammer 2. The UI builds its radio list from the model; `RebuildScene` clamps `lod_level_` against that model's own maximum, and a generator change re-clamps.
  - Multi drives the same chain the game would, impostor included: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`), capped by the compile-time `kMaxLods` (8).
- `badlands_game` reads `USE_BLOCKOUT_MODE` (any non-empty value) to render greybox proxies instead of detailed PBR materials.

## Debug overlays are shared, and the buffer belongs to the host
- **`badlands_game` and `badlands_ai_sandbox` both host `NavDebugOverlay` and `SkeletonDebugOverlay`** (`src/game/visual/`), each off by default and toggled in the debug panel. The apps differ only in the ground-height function they supply — flat `0` for the arena, `GroundAt` for the town.
- **Each host owns ONE `DebugLineBuffer` per frame**; overlays append and never assign `SceneContext::debug_lines`. See the invariant in `src/engine/CLAUDE.md` — getting this wrong makes one overlay silently erase another.
- **Skeletons draw OVER the capsules, which stay the character blockout mesh.** A `--screenshot` run cannot show them: `SaveScreenshot` calls `Update(0.0f)`, so the sim never ticks and no character has been projected yet. The overlay's wiring is covered headlessly by `badlands_animation_tests` (`[overlay]`) instead.

## badlands_viewer holds TWO AppViews, chosen at startup
- **`ModelViewerView` is the default; `--character` selects `CharacterViewerView`.** `SdlViewerApp` takes a view FACTORY, not a switchable view, so there is no in-session toggle between foliage and characters.
- **`--clip <logical-name>` and `--anim-time <0..1>` also imply `--character`.** The name is a key in the manifest (`assets/characters/quaternius/clips.json`), e.g. `walk`, not a filename; `--anim-time` pins playback so a `--screenshot` frame is reproducible.
```sh
scripts/screenshot.sh badlands_viewer /tmp/c.png --character --clip walk --anim-time 0.5
```
- **`ModelViewerView`'s generator list is: sphere, then `TreeCatalog()`, then one entry per prop** discovered under `assets/models/*/*.usdc` and **sorted by name** — `--generator <n>` indexes it, so an unsorted order would change which model a headless screenshot captures. The `.usdc` parses lazily inside `generate` (7 MB / 100k tris each); materials resolve eagerly via the `MaterialLibrary` cache.
- **A prop's prims are merged into ONE mesh** (treasure_chest is 5, rock_moss_set_01 is 6, all sharing one pack). That merge is a viewer presentation choice — `BuildImportedModels` still returns the list.
- **The prop's LOD chain is cached per generator (`prop_preview_`), not rebuilt per LOD click.** Parsing a `.usdc` and welding + decimating it is ~a second, and `RebuildScene` runs on every radio change.
- **The character viewer needs no `Sim`.** It drives `src/engine/animation/` directly, which is the separation that layer exists to keep — the runtime knows nothing about EnTT, characters or badlands.
- The shipped Quaternius rig is 53 joints and is **grounded at y=0** (joint origins span y=[0.000, 1.513]), so a character placed at a terrain height needs no vertical offset.

## The AI sandbox is a puppet-master layer, and it is one-way
- **A MODE says what world to build and who is in it**; the app then ticks and draws it. Modes today: `duel` (default), `sneak`, `teleport`, selected with `--mode` (`--seed N` for duel).
- **Nothing in `game/` or `badlands_game_lib` knows a mode exists.** Every system under observation behaves exactly as it does in the real game.
- **Testability never outranks layering.** If a mode needs a hook inside the sim, that is the wrong design — build the setup from Commands the game already has.
- Modes are view-free and are tested headless by `badlands_ai_sandbox_tests`.
