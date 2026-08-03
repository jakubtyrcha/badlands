# src/executables/ — the apps

Each app owns an `AppView` and builds its scene from the world itself; the app shell
(`engine/app/sdl_viewer_app.hpp`) owns the window, GPU and loop.

| App | What it is |
|---|---|
| `badlands_game` | the game: symbolic-map terrain + biome PBR packs, town, heroes |
| `badlands_ai_sandbox` | the AI harness, driven by a `--mode` |
| `badlands_viewer` | model/LOD viewer, plus the character/skeleton viewer (`--character`) |
| `badlands_mapview` | the map tool (see `src/mapgen/CLAUDE.md`) |

## Common flags
Run from the repo root — `shaders/` and `assets/` resolve relative to cwd.
```sh
./build/badlands_game --screenshot out.png   # headless: render one frame to PNG
./build/badlands_game --record frames/       # headless: render a frame sequence
scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 5
```
- `badlands_viewer --lod`: 0 = original cards (now deferred alpha-cutout, not forward), 1–3 = voxel LODs L0..L2, **4 = Impostor** (the baked octahedral billboard, which took over the slot voxel L3 used to hold), 5 = Multi (instanced field). Multi drives the same chain the game does, impostor included: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`), capped by the compile-time `kMaxLods` (8). Voxel L3 is still BUILT (the field selects it between 70 and 130 preview metres) but is no longer reachable from the viewer's manual switch.
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
- **The character viewer needs no `Sim`.** It drives `src/engine/animation/` directly, which is the separation that layer exists to keep — the runtime knows nothing about EnTT, characters or badlands.
- The shipped Quaternius rig is 53 joints and is **grounded at y=0** (joint origins span y=[0.000, 1.513]), so a character placed at a terrain height needs no vertical offset.

## The AI sandbox is a puppet-master layer, and it is one-way
- **A MODE says what world to build and who is in it**; the app then ticks and draws it. Modes today: `duel` (default), `sneak`, `teleport`, selected with `--mode` (`--seed N` for duel).
- **Nothing in `game/` or `badlands_game_lib` knows a mode exists.** Every system under observation behaves exactly as it does in the real game.
- **Testability never outranks layering.** If a mode needs a hook inside the sim, that is the wrong design — build the setup from Commands the game already has.
- Modes are view-free and are tested headless by `badlands_ai_sandbox_tests`.
