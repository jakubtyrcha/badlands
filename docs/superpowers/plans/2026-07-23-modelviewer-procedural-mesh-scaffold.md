# Model Viewer Procedural-Mesh Scaffold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate app entry points under `src/executables/`, then turn `badlands_viewer` into a procedural-mesh scaffold — a generator registry (seeded with a cube-sphere), a UV-checker debug material, and a two-window ImGui split (mesh setup vs. rendering-debug/light setup).

**Architecture:** The move relocates each executable's `main` + app-specific `AppView` only; shared game-render/mapgen library code stays. The viewer drops its rock/building prefab catalog for a `std::vector<MeshGenerator>` (name + `std::function<TexturedMeshResult()>`), applying a new engine `MaterialLibrary::CheckerAlbedo` material to whichever mesh the selected generator produces. The checker's pixel fill is a pure, unit-tested helper; the GPU wrapper and viewer are smoke-tested via a headless `--screenshot`.

**Tech Stack:** C++/Dawn/SDL3, CMake+Ninja, Catch2 (amalgamated), ImGui, glm.

## Global Constraints

- Run all commands from the repo root (`shaders/`, `assets/` resolve relative to cwd).
- Engine layer is game-agnostic: no game types in `src/engine/` (CheckerAlbedo takes only `glm::vec3`/ints — OK).
- Reversed-Z is project-wide (`GLM_FORCE_DEPTH_ZERO_TO_ONE`); do not touch depth/pipeline conventions.
- Do NOT add ImGui debug controls beyond what this plan specifies (CLAUDE.md).
- `MaterialLibrary::CheckerAlbedo` is a pre-approved engine-interface addition; no other engine-interface changes.
- Builds are heavy but incremental; the first build after a CMakeLists change reconfigures automatically (`build/` is already ninja-configured). Every `cmake --build build` is expected to take minutes, not seconds.
- Colors passed to `SolidColor`/`CheckerAlbedo` are treated as raw sRGB bytes; deferred lighting re-linearizes G-buffer albedo (so gray `0.881` ≈ linear `0.75`).

---

### Task 1: Move app entry points to `src/executables/`

Relocate the 5 executables' `main` + app-specific `AppView` files. Behavior-preserving: no source logic changes, only file locations, self-referential include paths, and CMake paths. `noise_texgen` stays in `src/tools/`.

**Files:**
- Move (via `git mv`):
  - `src/viewer/{main_viewer.cpp, model_viewer_view.cpp, model_viewer_view.hpp}` → `src/executables/viewer/`
  - `src/game/main_game.cpp` → `src/executables/game/main_game.cpp`; `src/game/views/{game_view.cpp, game_view.hpp}` → `src/executables/game/`
  - `src/ai_sandbox/{main_ai_sandbox.cpp, ai_sandbox_view.cpp, ai_sandbox_view.hpp}` → `src/executables/ai_sandbox/`
  - `src/mapview/{main_mapview.cpp, map_view_view.cpp, map_view_view.hpp}` → `src/executables/mapview/`
  - `src/mapgen/main_patchgen.cpp` → `src/executables/patchgen/main_patchgen.cpp`
- Modify (include fixups): the 8 moved `.cpp` files that include their own moved header (listed in Step 2).
- Modify: `CMakeLists.txt` (app source lists at lines ~322-325, 333-334, 356-357, 399, 412-413).

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: moved files at `src/executables/<app>/…`; Task 4 edits `src/executables/viewer/*` and the viewer CMake list.

- [ ] **Step 1: Move the files with `git mv` (preserves history)**

```bash
mkdir -p src/executables/viewer src/executables/game src/executables/ai_sandbox src/executables/mapview src/executables/patchgen
git mv src/viewer/main_viewer.cpp            src/executables/viewer/main_viewer.cpp
git mv src/viewer/model_viewer_view.cpp      src/executables/viewer/model_viewer_view.cpp
git mv src/viewer/model_viewer_view.hpp      src/executables/viewer/model_viewer_view.hpp
git mv src/game/main_game.cpp                src/executables/game/main_game.cpp
git mv src/game/views/game_view.cpp          src/executables/game/game_view.cpp
git mv src/game/views/game_view.hpp          src/executables/game/game_view.hpp
git mv src/ai_sandbox/main_ai_sandbox.cpp    src/executables/ai_sandbox/main_ai_sandbox.cpp
git mv src/ai_sandbox/ai_sandbox_view.cpp    src/executables/ai_sandbox/ai_sandbox_view.cpp
git mv src/ai_sandbox/ai_sandbox_view.hpp    src/executables/ai_sandbox/ai_sandbox_view.hpp
git mv src/mapview/main_mapview.cpp          src/executables/mapview/main_mapview.cpp
git mv src/mapview/map_view_view.cpp         src/executables/mapview/map_view_view.cpp
git mv src/mapview/map_view_view.hpp         src/executables/mapview/map_view_view.hpp
git mv src/mapgen/main_patchgen.cpp          src/executables/patchgen/main_patchgen.cpp
```

(Empty `src/viewer/`, `src/ai_sandbox/` dirs may remain untracked — harmless; git tracks no empty dirs. `src/game/views/`, `src/mapview/`, `src/mapgen/` retain their non-moved files.)

- [ ] **Step 2: Fix the self-referential include paths in the moved `.cpp` files**

Each moved file includes its own header via a `src/`-rooted path; rewrite the prefix to `executables/…`. Apply these exact edits (old → new):

| File | Old include | New include |
|---|---|---|
| `src/executables/viewer/main_viewer.cpp` | `#include "viewer/model_viewer_view.hpp"` | `#include "executables/viewer/model_viewer_view.hpp"` |
| `src/executables/viewer/model_viewer_view.cpp` | `#include "viewer/model_viewer_view.hpp"` | `#include "executables/viewer/model_viewer_view.hpp"` |
| `src/executables/game/main_game.cpp` | `#include "game/views/game_view.hpp"` | `#include "executables/game/game_view.hpp"` |
| `src/executables/game/game_view.cpp` | `#include "game/views/game_view.hpp"` | `#include "executables/game/game_view.hpp"` |
| `src/executables/ai_sandbox/main_ai_sandbox.cpp` | `#include "ai_sandbox/ai_sandbox_view.hpp"` | `#include "executables/ai_sandbox/ai_sandbox_view.hpp"` |
| `src/executables/ai_sandbox/ai_sandbox_view.cpp` | `#include "ai_sandbox/ai_sandbox_view.hpp"` | `#include "executables/ai_sandbox/ai_sandbox_view.hpp"` |
| `src/executables/mapview/main_mapview.cpp` | `#include "mapview/map_view_view.hpp"` | `#include "executables/mapview/map_view_view.hpp"` |
| `src/executables/mapview/map_view_view.cpp` | `#include "mapview/map_view_view.hpp"` | `#include "executables/mapview/map_view_view.hpp"` |

All OTHER includes in these files (e.g. `#include "game/..."`, `#include "engine/..."`, `#include "mapview/biome_manifest.hpp"`) point at files that did NOT move and stay unchanged. `main_patchgen.cpp` includes no moved header — no edit.

- [ ] **Step 3: Update the CMake source-list paths**

In `CMakeLists.txt`, apply these exact path replacements (the surrounding `set(...)`/`add_executable(...)` structure is unchanged):

```
src/viewer/main_viewer.cpp          → src/executables/viewer/main_viewer.cpp
src/viewer/model_viewer_view.cpp    → src/executables/viewer/model_viewer_view.cpp
src/game/main_game.cpp              → src/executables/game/main_game.cpp
src/game/views/game_view.cpp        → src/executables/game/game_view.cpp
src/ai_sandbox/main_ai_sandbox.cpp  → src/executables/ai_sandbox/main_ai_sandbox.cpp
src/ai_sandbox/ai_sandbox_view.cpp  → src/executables/ai_sandbox/ai_sandbox_view.cpp
src/mapview/main_mapview.cpp        → src/executables/mapview/main_mapview.cpp
src/mapview/map_view_view.cpp       → src/executables/mapview/map_view_view.cpp
```

And the patchgen line:
```
add_executable(badlands_patchgen src/mapgen/main_patchgen.cpp)
→ add_executable(badlands_patchgen src/executables/patchgen/main_patchgen.cpp)
```

Leave `src/game/scene/building_scene.cpp` (still in the viewer list — removed later in Task 4), `src/mapview/biome_manifest.cpp`, and every other non-moved path untouched.

- [ ] **Step 4: Build all targets**

Run: `cmake --build build`
Expected: full build succeeds, all executables link. (No warnings/errors introduced by the move.)

- [ ] **Step 5: Verify apps still run headless + tests pass**

```bash
perl -e 'alarm 40; exec @ARGV' ./build/badlands_viewer  --screenshot /private/tmp/claude-501/-Users-jakub-repos-badlands-clone-2/97c9f3e0-fcbf-433e-a4ce-7d455ce56aaf/scratchpad/move_viewer.png
perl -e 'alarm 40; exec @ARGV' ./build/badlands_ai_sandbox --screenshot /private/tmp/claude-501/-Users-jakub-repos-badlands-clone-2/97c9f3e0-fcbf-433e-a4ce-7d455ce56aaf/scratchpad/move_sandbox.png
./build/badlands_patchgen --help 2>/dev/null || ./build/badlands_patchgen 2>/dev/null; echo "patchgen ran (exit $?)"
ctest --test-dir build --output-on-failure
```
Expected: each `--screenshot` writes a PNG and exits 0; `ctest` passes (same set as before the move).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: move app entry points + AppViews to src/executables/

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `BuildCheckerboardRgba8` pure helper + unit test

A pure, GPU-free function that fills an RGBA8 checkerboard buffer. This is the unit-testable core of the checker material; the GPU wrapper (Task 3) uploads its output.

**Files:**
- Create: `src/engine/rendering/checker_texture.hpp` (header-only `inline` function, mirrors the header-only style of `engine/rendering/decal_math.hpp`).
- Create: `src/engine/tests/checker_texture_tests.cpp` (Catch2, mirrors `src/engine/tests/decal_math_tests.cpp`).
- Modify: `CMakeLists.txt` (new `badlands_checker_texture_tests` target, modeled on `badlands_decal_math_tests` at lines ~635-646).

**Interfaces:**
- Consumes: nothing.
- Produces: `std::vector<uint8_t> badlands::BuildCheckerboardRgba8(glm::vec3 color_a, glm::vec3 color_b, int tiles, int texels)` — returns `texels*texels*4` bytes, RGBA8, alpha 255; tile `(ti,tj)` is `color_a` when `(ti+tj)` even else `color_b`. Task 3 calls this.

- [ ] **Step 1: Write the failing test**

Create `src/engine/tests/checker_texture_tests.cpp` (matches the repo convention: angle-bracket include, no `CATCH_CONFIG_MAIN` — the amalgamated `.cpp` provides `main`):
```cpp
// Pure-CPU tests for the RGBA8 checkerboard fill (engine/rendering/
// checker_texture.hpp), the GPU-free core of MaterialLibrary::CheckerAlbedo.
#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>

#include "engine/rendering/checker_texture.hpp"

using badlands::BuildCheckerboardRgba8;

TEST_CASE("checkerboard has RGBA8 size and opaque alpha") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  REQUIRE(px.size() == 4u * 4u * 4u);
  for (size_t i = 3; i < px.size(); i += 4) REQUIRE(px[i] == 255);
}

TEST_CASE("checkerboard alternates color_a / color_b by tile parity") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  auto rgb_at = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * 4 + x) * 4;
    return glm::ivec3(px[o], px[o + 1], px[o + 2]);
  };
  // tile size = 4/2 = 2 px. (0,0)=A red, (2,0)=B green, (0,2)=B green, (2,2)=A red.
  REQUIRE(rgb_at(0, 0) == glm::ivec3(255, 0, 0));
  REQUIRE(rgb_at(2, 0) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(0, 2) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(2, 2) == glm::ivec3(255, 0, 0));
}
```

- [ ] **Step 2: Create a stub header + the CMake target so the test builds and fails**

Create `src/engine/rendering/checker_texture.hpp`:
```cpp
#pragma once

// Pure, GPU-free RGBA8 checkerboard fill — the unit-testable core of
// MaterialLibrary::CheckerAlbedo's debug material. Header-only (mirrors
// engine/rendering/decal_math.hpp). Engine, game-agnostic.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands {

// Builds a `texels`x`texels` RGBA8 checkerboard (row-major, tightly packed,
// alpha=255). `tiles` tiles per side; tile (ti,tj) is `color_a` when (ti+tj)
// is even, else `color_b`. Colors are raw sRGB (quantized to 8-bit), matching
// SolidColor's convention. `tiles>=1`, `texels>=1` assumed.
inline std::vector<uint8_t> BuildCheckerboardRgba8(glm::vec3 color_a,
                                                   glm::vec3 color_b, int tiles,
                                                   int texels) {
  return {};  // stub — replaced in Step 4
}

}  // namespace badlands
```

Add the test target to `CMakeLists.txt` (place after the `badlands_decal_math_tests` block, ~line 646):
```cmake
# badlands_checker_texture_tests: Catch2 suite for the pure RGBA8 checkerboard
# fill (engine/rendering/checker_texture.hpp) — the GPU-free core of
# MaterialLibrary::CheckerAlbedo. Pure CPU, modeled on badlands_decal_math_tests.
add_executable(badlands_checker_texture_tests
    src/engine/tests/checker_texture_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_checker_texture_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_checker_texture_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_checker_texture_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_checker_texture_tests COMMAND badlands_checker_texture_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build --target badlands_checker_texture_tests && ./build/badlands_checker_texture_tests`
Expected: FAIL — `px.size() == 64` fails (stub returns empty, size 0).

- [ ] **Step 4: Implement the real fill**

Replace the stub body in `src/engine/rendering/checker_texture.hpp`:
```cpp
inline std::vector<uint8_t> BuildCheckerboardRgba8(glm::vec3 color_a,
                                                   glm::vec3 color_b, int tiles,
                                                   int texels) {
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t a[3] = {to_byte(color_a.r), to_byte(color_a.g), to_byte(color_a.b)};
  const uint8_t b[3] = {to_byte(color_b.r), to_byte(color_b.g), to_byte(color_b.b)};
  const int tile_px = std::max(1, texels / std::max(1, tiles));

  std::vector<uint8_t> out(static_cast<size_t>(texels) * texels * 4);
  for (int y = 0; y < texels; ++y) {
    const int tj = std::min(y / tile_px, tiles - 1);
    for (int x = 0; x < texels; ++x) {
      const int ti = std::min(x / tile_px, tiles - 1);
      const uint8_t* c = ((ti + tj) & 1) ? b : a;
      const size_t o = (static_cast<size_t>(y) * texels + x) * 4;
      out[o + 0] = c[0];
      out[o + 1] = c[1];
      out[o + 2] = c[2];
      out[o + 3] = 255;
    }
  }
  return out;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build --target badlands_checker_texture_tests && ./build/badlands_checker_texture_tests`
Expected: PASS (both test cases, all assertions).

- [ ] **Step 6: Commit**

```bash
git add src/engine/rendering/checker_texture.hpp src/engine/tests/checker_texture_tests.cpp CMakeLists.txt
git commit -m "feat(engine): pure RGBA8 checkerboard fill helper + tests

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `MaterialLibrary::CheckerAlbedo`

Add the approved engine method that wraps `BuildCheckerboardRgba8` into a cached deferred `normalmapped` material (checker albedo + matte ARM + flat-normal default). No new shader/factory — reuses the existing `normalmapped` factory exactly like `SolidColor`. Verified by compiling; the pixel logic is already covered by Task 2, and end-to-end rendering by Task 4's screenshot.

**Files:**
- Modify: `src/engine/rendering/material_library.hpp` (declare `CheckerAlbedo`; add `<map>`/`<tuple>` includes + a `checker_cache_` member).
- Modify: `src/engine/rendering/material_library.cpp` (implement `CheckerAlbedo`; include `checker_texture.hpp`).

**Interfaces:**
- Consumes: `BuildCheckerboardRgba8(...)` (Task 2); existing `UploadTexture2DWithMips`, `CreateSolidColorTexture`, `sampler_`, `pipeline_gen_`, `device_`, `queue_`, `factory_`.
- Produces: `DeferredMaterial MaterialLibrary::CheckerAlbedo(glm::vec3 color_a, glm::vec3 color_b, int tiles = 8, int texels = 512, float roughness = 1.0f)`. Task 4 calls this.

- [ ] **Step 1: Declare the method + cache in the header**

In `src/engine/rendering/material_library.hpp`, add `#include <map>` and `#include <tuple>` alongside the existing `<unordered_map>`/`<vector>` includes.

Add the declaration right after the `SolidColor(...)` declaration (after its doc comment, ~line 78):
```cpp
  // Returns a cached deferred `normalmapped` material whose albedo is a
  // procedurally-generated checkerboard (`tiles` tiles/side of `color_a` /
  // `color_b`, `texels` px/side), with a flat-normal default and a matte 1x1
  // ARM at `roughness`. The checkerboard (BuildCheckerboardRgba8) is uploaded
  // with a full mip chain via UploadTexture2DWithMips and sampled through the
  // shared trilinear+aniso sampler. Caches by (color_a, color_b, tiles, texels,
  // roughness). Colors are raw sRGB (same convention as SolidColor). A UV-debug
  // material — no new shader/factory, just a non-1x1 albedo.
  DeferredMaterial CheckerAlbedo(glm::vec3 color_a, glm::vec3 color_b,
                                 int tiles = 8, int texels = 512,
                                 float roughness = 1.0f);
```

Add the cache member next to `solid_cache_` (~line 172), keyed by packed-RGB(a), packed-RGB(b), tiles, texels, roughness-byte:
```cpp
  // key: (packed sRGB color_a, packed sRGB color_b, tiles, texels, roughness*255).
  // Stored InstanceParams own the albedo/ARM texture views (which keep their
  // textures alive) for the material's lifetime -- same ownership model as
  // solid_cache_.
  std::map<std::tuple<uint32_t, uint32_t, int, int, uint8_t>, InstanceParams>
      checker_cache_;
```

- [ ] **Step 2: Implement the method**

In `src/engine/rendering/material_library.cpp`, add `#include "engine/rendering/checker_texture.hpp"` (with the other `engine/rendering/...` includes). Add the definition immediately after `SolidColor`'s definition (after its closing brace, ~line 114):
```cpp
DeferredMaterial MaterialLibrary::CheckerAlbedo(glm::vec3 color_a,
                                                glm::vec3 color_b, int tiles,
                                                int texels, float roughness) {
  auto pack = [](glm::vec3 c) {
    auto to_byte = [](float v) {
      return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return (to_byte(c.r) << 16) | (to_byte(c.g) << 8) | to_byte(c.b);
  };
  const uint8_t rough = static_cast<uint8_t>(
      std::lround(std::clamp(roughness, 0.0f, 1.0f) * 255.0f));
  const auto key =
      std::make_tuple(pack(color_a), pack(color_b), tiles, texels, rough);

  auto it = checker_cache_.find(key);
  if (it == checker_cache_.end()) {
    const std::vector<uint8_t> pixels =
        BuildCheckerboardRgba8(color_a, color_b, tiles, texels);
    // The view keeps its texture alive (same contract as CreateSolidColorTexture),
    // so we keep only the view -- the LoadedTexture wrapper can drop here.
    const LoadedTexture albedo = UploadTexture2DWithMips(
        device_, queue_, *pipeline_gen_, static_cast<uint32_t>(texels),
        static_cast<uint32_t>(texels), pixels.data());

    InstanceParams params;
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "albedo",
        .view = albedo.view,
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    // 1x1 ARM: R=255 (AO=1), G=roughness*255, B=0 (non-metal) -- as SolidColor.
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "arm",
        .view = CreateSolidColorTexture(device_, queue_, 255, rough, 0, 255),
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    it = checker_cache_.emplace(key, std::move(params)).first;
  }

  return DeferredMaterial{.factory = factory_.get(), .params = it->second};
}
```

- [ ] **Step 3: Build the engine + a target that links it**

Run: `cmake --build build --target badlands_checker_texture_tests`
Expected: `badlands_engine` (which now compiles the new `CheckerAlbedo`) recompiles and links cleanly. (No behavior to assert here beyond compilation; the fill is tested in Task 2, rendering in Task 4.)

- [ ] **Step 4: Commit**

```bash
git add src/engine/rendering/material_library.hpp src/engine/rendering/material_library.cpp
git commit -m "feat(engine): MaterialLibrary::CheckerAlbedo UV-checker debug material

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Rework the model viewer into a procedural-mesh scaffold

Replace the rock/building prefab catalog with a `MeshGenerator` registry (seeded with the cube-sphere), apply the checker material, and split the UI into a "Mesh" window + the existing rendering-debug "Debug" window. Verified by build + headless `--screenshot`. (Files are at their post-Task-1 locations.)

**Files:**
- Modify (rewrite): `src/executables/viewer/model_viewer_view.hpp`
- Modify (rewrite): `src/executables/viewer/model_viewer_view.cpp`
- Modify: `src/executables/viewer/main_viewer.cpp` (`--prefab` → `--generator`, setter rename)
- Modify: `CMakeLists.txt` (drop `src/game/scene/building_scene.cpp` from the viewer list + trim the stale comment)

**Interfaces:**
- Consumes: `MaterialLibrary::CheckerAlbedo(...)` (Task 3); existing `GenerateSphereTexturedMesh`, `AddFloor`, `AddMeshEntity`, `MaterialLibrary::SolidColor`, `ApplyLightEnvironment`, `EditorUI::DrawDebugPanel`, `OrbitCameraController`.
- Produces: `badlands_viewer` renders the selected generator's mesh with the checker material; `void ModelViewerView::SetInitialGeneratorIndex(int)` replaces `SetInitialPrefabIndex`.

- [ ] **Step 1: Rewrite the header**

Replace the entire contents of `src/executables/viewer/model_viewer_view.hpp` with:
```cpp
#pragma once

// badlands_viewer's AppView: a procedural-mesh scaffold. An orbit camera around
// a single mesh produced by the selected generator, on a neutral gray floor,
// textured with a UV-checker debug material, lit by the simple LightEnvironment
// sun. generators_ is the extension point where future foliage/rock generators
// slot in. Lives in src/executables/viewer/ (an app, not the engine).

#include <functional>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // TexturedMeshResult
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_renderer.hpp"  // ShadowDebugMode
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class ModelViewerView : public AppView {
 public:
  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

  // Selects the generator shown once Initialize() builds the registry + scene.
  // Must be called before Initialize() -- main_viewer.cpp's `--generator <n>`
  // CLI arg uses it for headless screenshot verification. Out-of-range indices
  // are clamped in Initialize().
  void SetInitialGeneratorIndex(int index) { generator_index_ = index; }

  // Selects the initial ShadowDebugMode (headless `--shadow-debug <n>`:
  // 0=Off, 1=Combined, 2=ShadowMapOnly, 3=ContactOnly). Call before Initialize().
  void SetInitialShadowDebugMode(ShadowDebugMode mode) {
    initial_shadow_debug_mode_ = mode;
  }

 private:
  // A named procedural-mesh generator: produces one mesh added to the scene.
  struct MeshGenerator {
    std::string name;
    std::function<TexturedMeshResult()> generate;
  };

  void BuildGenerators();
  // Re-derives env_'s sky/SH/sun into scene_context_ and mirrors it into scene_.
  void ApplyEnvironment();
  // Fresh graph: re-mirror lighting, add the gray floor, run the selected
  // generator, apply checker_mat_, reframe the orbit on the mesh bounds.
  void RebuildScene();

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  OrbitCameraController orbit_;

  std::vector<MeshGenerator> generators_;
  int generator_index_ = 0;
  DeferredMaterial checker_mat_;  // UV-checker debug material for the object

  ShadowDebugMode initial_shadow_debug_mode_ = ShadowDebugMode::Off;

  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
```

- [ ] **Step 2: Rewrite the implementation**

Replace the entire contents of `src/executables/viewer/model_viewer_view.cpp` with:
```cpp
#include "executables/viewer/model_viewer_view.hpp"

#include <algorithm>
#include <utility>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/sdl_input_util.hpp"  // NormalizedWheelY
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"

namespace badlands {

namespace {

// Flat light-gray debug floor: rgb pre-encoded so that, after deferred lighting
// re-linearizes the non-sRGB albedo, the surface lands near linear 0.75
// reflectance. Roughness maxed to keep it diffuse so shadows read clearly.
constexpr glm::vec3 kFloorGray{0.881f};
constexpr float kFloorRoughness = 1.0f;
constexpr float kFloorSize = 40.0f;
// One floor-UV repeat per ~2 world units instead of stretching one copy.
constexpr float kFloorUvRepeatSpacing = 2.0f;

}  // namespace

bool ModelViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("ModelViewerView::Initialize: MaterialLibrary init failed");
    return false;
  }

  BuildGenerators();
  if (generators_.empty()) {
    spdlog::error("ModelViewerView::Initialize: empty generator registry");
    return false;
  }
  generator_index_ =
      std::clamp(generator_index_, 0, static_cast<int>(generators_.size()) - 1);

  // UV-checker debug material (two distinct grays) for the generated object, so
  // its UVs read against the flat gray floor.
  checker_mat_ = matlib_.CheckerAlbedo(glm::vec3(0.85f), glm::vec3(0.35f));

  ApplyEnvironment();
  RebuildScene();
  scene_renderer_->SetShadowDebugMode(initial_shadow_debug_mode_);

  if (!matlib_.ok()) {
    spdlog::error("ModelViewerView::Initialize: material load failed");
    return false;
  }
  return true;
}

void ModelViewerView::BuildGenerators() {
  generators_.clear();
  // The "test" generator: the engine's cube-sphere (cube -> 16x16 per face ->
  // normalized sphere, EAC UVs). Future foliage/rock generators append here.
  generators_.push_back(
      {.name = "Sphere (test)",
       .generate = [] { return GenerateSphereTexturedMesh(1.0f, 16); }});
}

void ModelViewerView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void ModelViewerView::RebuildScene() {
  // Fresh graph drops every prior entity; its ctor resets sun/ambient to
  // SceneGraph defaults, so re-mirror scene_context_'s derived lighting.
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor(scene_, kFloorSize, matlib_.SolidColor(kFloorGray, kFloorRoughness),
           kFloorSize / kFloorUvRepeatSpacing);

  TexturedMeshResult mesh = generators_[generator_index_].generate();
  const Aabb bounds = mesh.local_bounds;
  AddMeshEntity(scene_, "mesh", std::move(mesh), checker_mat_);

  const glm::vec3 center = bounds.Center();
  const float radius = glm::length(bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
}

void ModelViewerView::HandleEvent(const SDL_Event& event, int /*width*/,
                                  int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = true;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (left_mouse_down_) {
        orbit_.HandleMouseDrag(event.motion.xrel, event.motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      orbit_.HandleMouseWheel(NormalizedWheelY(event.wheel));
      break;
    default:
      break;
  }
}

void ModelViewerView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
}

void ModelViewerView::DrawUI() {
  if (!scene_renderer_ || generators_.empty()) return;

  // Mesh-setup window: single-select generator list.
  int selected = generator_index_;
  ImGui::Begin("Mesh");
  for (int i = 0; i < static_cast<int>(generators_.size()); ++i) {
    if (ImGui::Selectable(generators_[i].name.c_str(), i == generator_index_)) {
      selected = i;
    }
  }
  ImGui::End();

  if (selected != generator_index_) {
    generator_index_ = selected;
    RebuildScene();
  }

  // Visual-setup window: the shared rendering-debug + light editor ("Debug").
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }
}

void ModelViewerView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
```

- [ ] **Step 3: Update the CLI in `main_viewer.cpp`**

In `src/executables/viewer/main_viewer.cpp`: change the comment + variable so `--prefab` becomes `--generator`, and call the renamed setter. Apply these edits:

Replace the `int prefab_index = 0;` line's comment + declaration:
```cpp
  // Selects the initial generator shown, so a `--screenshot` run can capture a
  // specific mesh without driving the ImGui list. Not part of the stable CLI.
  int generator_index = 0;
```
Replace the `--prefab` parse branch:
```cpp
    if (std::strcmp(argv[i], "--generator") == 0 && i + 1 < argc) {
      generator_index = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--shadow-debug") == 0 && i + 1 < argc) {
```
Replace the capture + setter call:
```cpp
  return app.Run(argc, argv, [generator_index, shadow_debug_mode](
                                 const badlands::RenderContext& /*ctx*/) {
    auto view = std::make_unique<badlands::ModelViewerView>();
    view->SetInitialGeneratorIndex(generator_index);
    view->SetInitialShadowDebugMode(shadow_debug_mode);
    return view;
  });
```

- [ ] **Step 4: Drop `building_scene.cpp` from the viewer CMake list**

In `CMakeLists.txt`, the viewer source list becomes exactly:
```cmake
set(badlands_app_sources_viewer
    src/executables/viewer/main_viewer.cpp
    src/executables/viewer/model_viewer_view.cpp)
```
Also trim the now-stale comment block above it that describes `ModelViewerView` reusing `AddBuildingToScene`/`building_scene.cpp` (replace with a one-line note that the three apps each grow their own `AppView`). Note: `src/game/scene/building_scene.{cpp,h}` is left in the tree (now compiled by no target — grep confirmed it was viewer-only); deleting it is out of scope.

- [ ] **Step 5: Build the viewer**

Run: `cmake --build build --target badlands_viewer`
Expected: compiles + links cleanly (no references to the removed prefab types/includes remain).

- [ ] **Step 6: Headless screenshot smoke**

```bash
perl -e 'alarm 40; exec @ARGV' ./build/badlands_viewer --screenshot /private/tmp/claude-501/-Users-jakub-repos-badlands-clone-2/97c9f3e0-fcbf-433e-a4ce-7d455ce56aaf/scratchpad/viewer_checker.png
```
Expected: exit 0, PNG written. Open the PNG and confirm: a checkerboarded sphere sits on the flat gray floor, lit (shadow reads), and the checker pattern (UVs) is visible.

- [ ] **Step 7: Full build + tests (regression)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: everything builds; all tests pass (including `badlands_checker_texture_tests`).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(viewer): procedural-mesh scaffold (generator registry + checker material + Mesh/Debug windows)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** Move → Task 1. Generator registry + single-select replace + cube-sphere seed → Task 4. Gray floor + simple `LightEnvironment` kept → Task 4 (`RebuildScene`/`ApplyEnvironment`). Checker via generated albedo + `CheckerAlbedo` helper → Tasks 2-3, applied in Task 4. Two windows ("Mesh" + existing "Debug") → Task 4 `DrawUI`. `--generator` CLI → Task 4. Prefab removal → Task 4.
- **Deviation from spec (flagged):** the spec said `building_scene.cpp` "stays for the game app," but grep shows it was compiled ONLY into the viewer; after prefab removal it's compiled by no target. Plan leaves the files in place (not deleted) and drops them from the viewer list — surfaced in Task 4 Step 4 for the user to decide on later cleanup.
- **Deviation from spec (minor):** the "Visual" window keeps its existing ImGui title "Debug" rather than being renamed, because `DrawDebugPanel` is shared engine code and renaming its window would change other apps' titles. The two-window separation the spec asked for is still delivered ("Mesh" + "Debug"). No engine `editor_ui` change.
- **Type consistency:** `SetInitialGeneratorIndex` / `generator_index_` / `generators_` / `MeshGenerator{name,generate}` / `checker_mat_` used consistently across the header + cpp. `CheckerAlbedo` signature identical in header decl, cpp def, and Task 4 call site.
