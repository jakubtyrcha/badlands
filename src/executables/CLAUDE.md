# src/executables/ — the apps

Each app owns an `AppView` and builds its scene from the world itself; the app shell
(`engine/app/sdl_viewer_app.hpp`) owns the window, GPU and loop.

| App | What it is |
|---|---|
| `badlands_game` | the game: symbolic-map terrain + biome PBR packs, town, heroes |
| `badlands_ai_sandbox` | the AI harness, driven by a `--mode` |
| `badlands_viewer` | model/LOD viewer |
| `badlands_mapview` | the map tool (see `src/mapgen/CLAUDE.md`) |

## Common flags
Run from the repo root — `shaders/` and `assets/` resolve relative to cwd.
```sh
./build/badlands_game --screenshot out.png   # headless: render one frame to PNG
./build/badlands_game --record frames/       # headless: render a frame sequence
scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 5
```
- `badlands_viewer --lod`: 0 = original cards, 1–4 = voxel LODs L0..L3, 5 = Multi (instanced field). L3 is the coarse tail (~230–680 tris/tree). Multi drives the same L0..L3 chain: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`), capped by the compile-time `kMaxLods` (8).
- `badlands_game` reads `USE_BLOCKOUT_MODE` (any non-empty value) to render greybox proxies instead of detailed PBR materials.

## The AI sandbox is a puppet-master layer, and it is one-way
- **A MODE says what world to build and who is in it**; the app then ticks and draws it. Modes today: `duel` (default), `sneak`, `teleport`, selected with `--mode` (`--seed N` for duel).
- **Nothing in `game/` or `badlands_game_lib` knows a mode exists.** Every system under observation behaves exactly as it does in the real game.
- **Testability never outranks layering.** If a mode needs a hook inside the sim, that is the wrong design — build the setup from Commands the game already has.
- Modes are view-free and are tested headless by `badlands_ai_sandbox_tests`.
