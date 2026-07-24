# Model Viewer → Procedural-Mesh Scaffold

Date: 2026-07-23

## Goal

Turn `badlands_viewer` from a rock/building prefab browser into scaffolding for
**procedural mesh generation** (the future home of foliage, rocks, etc.). The
generation itself is out of scope — this delivers the frame it slots into: a
generator registry, a lit scene with a floor, a UV-checker debug material, and a
two-window ImGui split (mesh setup vs. visual/rendering debug).

Bundled with a preparatory refactor: consolidate app entry points under
`src/executables/`.

## Part 1 — Move apps to `src/executables/`

Move each executable's **entry point + app-specific files only**. Shared
library/render code stays where it is.

| From | To |
|---|---|
| `src/viewer/{main_viewer.cpp, model_viewer_view.cpp, model_viewer_view.hpp}` | `src/executables/viewer/` |
| `src/game/main_game.cpp` + `src/game/views/game_view.{cpp,hpp}` | `src/executables/game/` |
| `src/ai_sandbox/{main_ai_sandbox.cpp, ai_sandbox_view.cpp, ai_sandbox_view.hpp}` | `src/executables/ai_sandbox/` |
| `src/mapview/{main_mapview.cpp, map_view_view.cpp, map_view_view.hpp}` | `src/executables/mapview/` |
| `src/mapgen/main_patchgen.cpp` | `src/executables/patchgen/` |

**Stays put (NOT moved):**
- `src/game/{arena,building_catalog,creature_manifest,factors_manifest,scenario,material_pack,material_id,...}` and the subdirs `scene/`, `visual/`, `geometry/`, `map/`, `ui/` — the game render layer + data manifests.
- `src/mapgen/` — the mapgen library.
- `src/mapview/biome_manifest.{cpp,hpp}` — shared with the game app.
- `src/tools/noise_texgen/` — **stays** (per decision; a headless CPU tool, left in `src/tools/`).
- `src/engine`, `src/core`, `src/crates` — untouched.

### Mechanics
- **Includes:** every moved file includes its own header via a `src/`-rooted
  path (e.g. `#include "viewer/model_viewer_view.hpp"`). All such includes are
  self-referential (each `main` includes its own view; each view includes its
  own header). Rewrite them to the new `executables/...` prefix, e.g.
  `#include "executables/viewer/model_viewer_view.hpp"`. Verified reference set:
  - `src/viewer/main_viewer.cpp`, `src/viewer/model_viewer_view.cpp` → `viewer/...`
  - `src/game/main_game.cpp`, `src/game/views/game_view.cpp` → `game/views/game_view.hpp`
  - `src/ai_sandbox/main_ai_sandbox.cpp`, `src/ai_sandbox/ai_sandbox_view.cpp` → `ai_sandbox/...`
  - `src/mapview/main_mapview.cpp`, `src/mapview/map_view_view.cpp` → `mapview/map_view_view.hpp`
  - `main_patchgen.cpp` includes no moved header (self-contained).
  The `${CMAKE_SOURCE_DIR}/src` include root already on the targets keeps
  `executables/...` resolvable — no new include dir needed.
- **CMake:** update the `set(badlands_app_sources_* ...)` lists, the
  `add_executable(badlands_patchgen ...)` path, and the loop over
  `viewer/game/ai_sandbox` to point at the new locations. Note the viewer's
  extra TU `src/game/scene/building_scene.cpp` and the game/mapview extra TUs
  (visual/geometry/ui helpers) stay at their current paths — only the moved
  files' paths change.
- The `game_view.hpp` still `#include`s `game/...` render-layer headers (which
  did **not** move); those includes are unchanged.

### Verification
- `cmake --build build` — all targets link.
- `ctest --test-dir build` — existing tests pass.
- Headless smoke each app that has a `--screenshot` path (viewer/game/ai_sandbox/
  mapview) + run `badlands_patchgen` once.

## Part 2 — Model viewer becomes a procedural-mesh scaffold

### Generator registry (app-side, in `src/executables/viewer/`)

```cpp
struct MeshGenerator {
  std::string name;
  std::function<TexturedMeshResult()> generate;  // produces the mesh
};
```

- Held as `std::vector<MeshGenerator>`, built once in `Initialize`.
- Seed entry: `{"Sphere (test)", []{ return GenerateSphereTexturedMesh(1.0f, 16); }}`
  — the existing engine gen (cube → 16×16-per-face → normalized sphere, EAC UVs).
- Future foliage/rock generators append here; that append is the whole point of
  the scaffold. No per-generator material/parameters yet (YAGNI).

### Scene

- **Floor:** unchanged — the existing gray `SolidColor` debug floor
  (`AddFloor(scene, kFloorSize, matlib_.SolidColor(kDebugGray, ...), ...)`).
  This is "the flat floor mesh with the debug material".
- **Light:** unchanged — the existing `LightEnvironment` sun + gradient sky
  ("keep simple sun"; no day/night cycle). `ApplyEnvironment()` /
  sun-mirroring logic stays as-is.
- **Object:** the single mesh produced by the selected generator, at the
  origin, with the **checker** debug material.
- **Selection = replace** (single object). Selecting a generator runs
  `RebuildScene()`: fresh `SceneGraph`, re-mirror lighting, add floor, run the
  selected generator, apply the checker material via `AddMeshEntity`, then
  `orbit_.FrameBounds(bounds.Center(), radius)` + `UpdateCamera`.

The old `PrefabEntry`/`PrefabCategory`/`BuildCatalog`/`AddPrefab` machinery and
the rock/building includes (`game/building_catalog.h`, `game/scene/building_scene.h`,
`game/geometry/ploppable_rings.h`, extrusion/ploppable ring logic) are removed
from the viewer. `building_scene.cpp` stays in `src/game/scene/` for the game
app; it's simply no longer compiled into `badlands_viewer` (drop it from the
viewer's CMake source list).

### Checker debug material — `MaterialLibrary::CheckerAlbedo` (engine addition, approved)

Add one additive method to `MaterialLibrary` (sibling to `SolidColor`), engine
& game-agnostic:

```cpp
// Returns a cached deferred `normalmapped` material whose albedo is a
// procedurally-generated checkerboard (N tiles/side of color_a / color_b),
// with a flat-normal default and a matte 1x1 ARM at `roughness`. The
// checkerboard is CPU-built (RGBA8) and uploaded with a full mip chain via
// UploadTexture2DWithMips, sampled through the library's shared trilinear +
// aniso sampler. Caches by (color_a, color_b, tiles, texels, roughness).
DeferredMaterial CheckerAlbedo(glm::vec3 color_a, glm::vec3 color_b,
                               int tiles = 8, int texels = 512,
                               float roughness = 1.0f);
```

- **No new material type / shader** — reuses the existing `normalmapped` kDeferred
  factory + `texture_overrides`, exactly like `SolidColor`. Only the albedo
  source differs (an N×N checker instead of a 1×1 solid).
- Implementation mirrors `SolidColor`: build the albedo texture (here via a
  small CPU checker fill + `UploadTexture2DWithMips(device_, queue_, *pipeline_gen_, ...)`),
  reuse the flat-normal default + a 1×1 ARM from `CreateSolidColorTexture`, bind
  through the shared `sampler_`. `MaterialLibrary` already stores `pipeline_gen_`
  (member), `device_`, `queue_`, and `sampler_`, so the upload path has
  everything it needs — no new state. Colors follow `SolidColor`'s sRGB-byte
  convention (deferred lighting re-linearizes G-buffer albedo).
- Cache keyed on the tuple so repeated calls reuse one texture pair.
- Viewer calls `matlib_.CheckerAlbedo(glm::vec3(0.85f), glm::vec3(0.25f))` (two
  distinct grays) for the generated object, so its UVs read clearly against the
  flat gray floor.

### UI — two windows

Replaces the current `DrawUI` (a "Prefab" combo + `DrawDebugPanel`).

- **"Mesh" window** — single-select list (`ImGui::Selectable` per generator
  name, or a combo). Changing selection sets `generator_index_` and calls
  `RebuildScene()`. This is the mesh-setup surface.
- **"Visual" window** — the existing `EditorUI::DrawDebugPanel(env_,
  *scene_renderer_, dt_)` (G-buffer/shadow/fog debug selectors + the
  `LightEnvironment` sun editor + FPS). This is the visual-setup surface; when
  it reports `env_changed`, call `ApplyEnvironment()` as today. (Optionally pass
  a "Visual" window title; `DrawDebugPanel` currently names its window "Debug" —
  a cosmetic rename only, no behavior change.)

### CLI / headless
- Keep a `--generator <n>` selector analogous to the old `--prefab <n>` (used by
  `--screenshot` headless verification). Drop `--prefab`. `--shadow-debug` stays.

### Out of scope (explicit)
- Real procedural generation algorithms (foliage/rock meshing).
- Per-generator materials or parameters; additive multi-object scenes.
- Day/night (`DaylightConfig`/`SimClock`) — the simple `LightEnvironment` stays.
- Any new material *shader* / factory type.
- ImGui knobs beyond the generator list + the existing debug panel (per CLAUDE.md:
  no unrequested debug controls).

## Verification (Part 2)
- Build + `ctest` (including a `CheckerAlbedo` sanity path if a geometry/material
  test target is the natural home; otherwise covered by the smoke render).
- `./build/badlands_viewer --screenshot out.png` renders the checker sphere on
  the gray floor, lit; visually confirm the checker (UVs) reads and the two
  windows appear.
