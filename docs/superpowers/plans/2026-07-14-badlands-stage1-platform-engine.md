# Badlands Stage 1 — Platform + Core Engine Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up badlands as a bare-bones C++/Dawn/SDL3 app that renders a lit, textured, mipmapped sphere through sampo's ported core engine, with the Rust feature-libs (noiser/nav/wesl/assets) linked via Corrosion and a single swappable `build_test_scene()` seam.

**Architecture:** Re-platform from a Rust-hosted (winit/wgpu) app to a C++-hosted (SDL3 + Dawn + tint) app. Port sampo's engine (lower-level API + data-driven material system + scene graph + forward-opaque pass + tonemap) into `src/engine/`; keep substantial Rust dependency-backed features (`noiser`, `nav`, `wesl`, `assets`) as Corrosion-linked static libs under `src/crates/`. The game layer is NOT wired this stage — the scene is populated by one placeholder `build_test_scene()`.

**Tech Stack:** C++23, Dawn (WebGPU, from-source), tint (Dawn's WGSL compiler), SDL3, glm, EnTT, spdlog, CMake + FetchContent + Corrosion; Rust crates (`wesl` 0.4, `image` 0.25 jpeg, `gltf` 1.4, plus the existing `nav`/`noiser`). Design spec: `docs/superpowers/specs/2026-07-14-badlands-cpp-replatform-design.md`.

## Global Constraints
- **C++ standard:** `c++23` (match sampo).
- **Dawn:** built from source, pinned — `DAWN_VERSION 7866`, SHA `cae082e16781493ee0f709975b8ed2a6e72a20b3`, mirror `https://dawn.googlesource.com/dawn` (copied verbatim from `../sampo/CMakeLists.txt`). Metal backend on macOS (`darwin`).
- **Rust↔CMake linking:** Corrosion only — `corrosion_import_crate(MANIFEST_PATH …)` + `corrosion_set_env_vars(<crate> "RUSTFLAGS=-Cpanic=unwind")`. Rust crates are `crate-type = ["staticlib","rlib"]`.
- **No game types in `src/engine/` or `src/core/`.** The only game-specific symbol this stage is `build_test_scene()` in `src/main.cpp`.
- **Ownership convention for ports:** copy the sampo source file(s), change namespace `sampo::` → `badlands::`, rewrite `#include "…"` to badlands paths, and pull in transitive support headers until it compiles. A "port step" is done when the file compiles/links in badlands and the task's verification passes. Do NOT re-architect ported code this stage.
- **Reference build:** the current Rust app on branch `building-placement-mechanic` (uncommitted) is the behavioral/visual reference; do not delete it until Stage 3 parity (it simply stops being built once CMake is the entry).
- **Commits:** frequent, one per task step group; end each task with a commit. Co-author trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## File structure (created/modified this stage)
```
CMakeLists.txt                     # NEW top-level build (SDL3+Dawn+Corrosion), replaces cargo as entry
cmake/FetchDawn.cmake              # NEW (cribbed from sampo) Dawn from-source
src/main.cpp                       # NEW app entry: platform + engine + build_test_scene() seam
src/engine/                        # NEW ported engine (rendering/material/scene/passes/shaders)
src/core/                          # NEW generic C++ (math glue, GeometryType, small utils)
src/crates/wesl/                   # NEW Rust crate: .wesl -> WGSL C ABI
src/crates/assets/                 # NEW Rust crate: JPEG decode + glTF parse C ABI
src/crates/nav/                    # NEW Rust crate: extracted from src/nav.rs (GamePathfinder C ABI)
shaders/                           # engine shaders (frame, material/*, passes/tonemapping, compute/mip_generator*)
game/                              # UNTOUCHED this stage (migrated in Stage 2)
```
The old Rust host (`src/app.rs`, `src/gpu/`, `src/scene/`, `src/ui/`, `src/main.rs`, `Cargo.toml` bin) is left in place but no longer built by CMake; deletion happens as Stage 2/3 replace its pieces.

---

## Task A1: CMake skeleton + SDL3 window

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/main.cpp`
- Create: `.gitignore` entry for `build/`

**Interfaces:**
- Produces: a `badlands` executable target; an SDL3 window opened in `main()`.

- [ ] **Step 1: Write `CMakeLists.txt` (project + SDL3 + exe)**
```cmake
cmake_minimum_required(VERSION 3.24)
project(badlands LANGUAGES C CXX OBJC OBJCXX)   # OBJC/OBJCXX: Dawn Metal backend on Apple
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# SDL3 (crib from ../sampo/CMakeLists.txt lines ~28-35)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.0)
FetchContent_MakeAvailable(SDL3)

add_executable(badlands src/main.cpp)
target_link_libraries(badlands PRIVATE SDL3::SDL3)
target_include_directories(badlands PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

- [ ] **Step 2: Write `src/main.cpp` (open a window, run an event loop)**
```cpp
#include <SDL3/SDL.h>
int main(int, char**) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window* window = SDL_CreateWindow("badlands", 1600, 900, SDL_WINDOW_RESIZABLE);
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) running = false;
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
```

- [ ] **Step 3: Configure + build**
Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: `build/badlands` produced, no errors.

- [ ] **Step 4: Run and verify a window opens**
Run: `./build/badlands` (close the window to exit)
Expected: a 1600×900 "badlands" window appears and closes cleanly.

- [ ] **Step 5: Commit**
```bash
echo "build/" >> .gitignore
git add CMakeLists.txt src/main.cpp .gitignore
git commit -m "build: CMake + SDL3 window skeleton"
```

---

## Task A2: Dawn device + surface; clear the window

**Files:**
- Create: `cmake/FetchDawn.cmake` (crib `../sampo/CMakeLists.txt` Dawn section + its `FetchDawnSource` handling verbatim, incl. the pinned SHA from Global Constraints)
- Port: `../sampo/src/rendering/gpu_context.{hpp,cpp}` → `src/engine/rendering/gpu_context.{hpp,cpp}` (namespace → `badlands`)
- Modify: `CMakeLists.txt` (Dawn + link `webgpu`, add engine sources), `src/main.cpp`

**Interfaces:**
- Produces: `badlands::GpuContext` — owns `wgpu::Device`, `wgpu::Queue`, `wgpu::Surface`; `AcquireSurfaceTexture()`, `Configure(w,h)`. `main` clears the surface each frame.

- [ ] **Step 1: Add Dawn to CMake** — `include(cmake/FetchDawn.cmake)`; link `webgpu` to `badlands`. (Copy sampo's exact `FetchContent_Declare(dawn …)` + `DAWN_VERSION`/`DAWN_PINNED_SHA` block.)
- [ ] **Step 2: Port `gpu_context.{hpp,cpp}`** — copy from sampo, adapt namespace/includes; keep the SDL3-surface creation path (sampo creates the `wgpu::Surface` from the SDL window — copy that helper too, e.g. `sdl3webgpu` or its inline equivalent). Add both files to the `badlands` target sources.
- [ ] **Step 3: Wire `main.cpp`** — create `badlands::GpuContext` from the SDL window; per frame: acquire surface texture, begin a render pass that clears to a color (e.g. `{0.02,0.02,0.03,1}`), submit, present.
- [ ] **Step 4: Build**
Run: `cmake --build build` — Expected: links `webgpu` + SDL3, no errors. (First Dawn build is slow.)
- [ ] **Step 5: Run + verify clear color**
Run: `./build/badlands` — Expected: the window shows the solid clear color, resizes cleanly, closes cleanly.
- [ ] **Step 6: Commit**
```bash
git add cmake/FetchDawn.cmake src/engine/rendering/gpu_context.* src/main.cpp CMakeLists.txt
git commit -m "engine: Dawn device + surface; clear the window"
```

---

## Task B1: `wesl` Rust crate (.wesl → WGSL) via Corrosion

**Files:**
- Create: `src/crates/wesl/Cargo.toml`, `src/crates/wesl/src/lib.rs`, `src/crates/wesl/include/badlands_wesl.h`
- Modify: `CMakeLists.txt` (Corrosion + import + link)
- Test: `src/crates/wesl/src/lib.rs` (`#[cfg(test)]`)

**Interfaces:**
- Produces (C ABI): `char* badlands_wesl_compile(const char* shader_dir, const char* module_path);` returns malloc'd NUL-terminated WGSL or NULL on error; `void badlands_wesl_free(char*);`. Rust side wraps the `wesl` 0.4 API used in the current `src/gpu/pipelines.rs` (`Wesl::new(dir).compile(ModulePath)`).

- [ ] **Step 1: Write the failing Rust test** (`src/crates/wesl/src/lib.rs`)
```rust
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn compiles_frame_module() {
        // shaders/common/frame.wesl must exist relative to the repo root.
        let wgsl = compile_to_wgsl("shaders", "common/frame");
        assert!(wgsl.expect("compile ok").contains("struct FrameUniforms"));
    }
}
```
- [ ] **Step 2: Cargo.toml** (staticlib+rlib; `wesl = "0.4"`; `[profile.*] panic="abort"` — Corrosion overrides to unwind at link).
- [ ] **Step 3: Implement `compile_to_wgsl(dir, module) -> Result<String,String>`** porting the WESL invocation from `src/gpu/pipelines.rs::compile_wesl` (ModulePath Absolute, `wesl.compile(...).to_string()`), plus the `extern "C"` `badlands_wesl_compile`/`_free` thunks (CString in, `CString::into_raw` out, `catch_unwind`).
- [ ] **Step 4: Run the Rust test** — `cargo test -p wesl` — Expected: PASS.
- [ ] **Step 5: Write `include/badlands_wesl.h`** (the two prototypes above).
- [ ] **Step 6: Corrosion-link** — in `CMakeLists.txt`: FetchContent Corrosion; `corrosion_import_crate(MANIFEST_PATH src/crates/wesl/Cargo.toml)`; `corrosion_set_env_vars(wesl "RUSTFLAGS=-Cpanic=unwind")`; `target_link_libraries(badlands PRIVATE wesl)`; add `src/crates/wesl/include` to includes.
- [ ] **Step 7: C++ smoke** — in `main.cpp` (temporary), call `badlands_wesl_compile("shaders","common/frame")`, assert non-null, log the first line, free it. Build + run.
Run: `cmake --build build && ./build/badlands` — Expected: log shows compiled WGSL; no crash. (Remove the temporary smoke before commit or guard behind `--smoke`.)
- [ ] **Step 8: Commit**
```bash
git add src/crates/wesl CMakeLists.txt src/main.cpp
git commit -m "crates: wesl (.wesl->WGSL) via Corrosion"
```

---

## Task B2: `assets` Rust crate (JPEG decode + glTF parse) via Corrosion

**Files:**
- Create: `src/crates/assets/{Cargo.toml,src/lib.rs,include/badlands_assets.h}`
- Modify: `CMakeLists.txt`
- Test: `src/crates/assets/src/lib.rs`

**Interfaces:**
- Produces (C ABI): `BadlandsImage badlands_decode_jpeg(const char* path);` where `struct BadlandsImage { uint8_t* rgba; uint32_t width, height; };` (rgba is malloc'd, width/height 0 on failure); `void badlands_image_free(BadlandsImage);`. Plus `BadlandsGltfTextures badlands_gltf_pack_textures(const char* gltf_path);` returning three URI strings (base/normal/mr) — port `load_pack_uris` from the current `src/scene/material.rs` (URI, not name).

- [ ] **Step 1: Failing Rust tests** — `decode_jpeg` on `assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg` returns `1024×1024`; `gltf_pack_textures` resolves base=`_diff`, normal=`_nor_gl`, mr=`_arm` (reuse the logic + assertions from `src/scene/material.rs` tests).
- [ ] **Step 2: Cargo.toml** — staticlib+rlib; `image = { version="0.25", default-features=false, features=["jpeg"] }`, `gltf = { version="1.4", default-features=false }`.
- [ ] **Step 3: Implement** decode + gltf-uri resolution (port `decode_rgba`/`load_pack_uris` from `src/scene/material.rs`, **minus** mip generation — mips are GPU-side) + `extern "C"` thunks returning the structs above (`Box::into_raw`/leak the Vec via `into_raw_parts`; free reconstructs).
- [ ] **Step 4: Run Rust tests** — `cargo test -p assets` — Expected: PASS.
- [ ] **Step 5: Header + Corrosion-link** (mirror B1 steps 5-6 for `assets`).
- [ ] **Step 6: C++ smoke** — decode one JPEG, assert `width==1024`, free. Build + run.
- [ ] **Step 7: Commit** `git commit -m "crates: assets (jpeg decode + gltf parse) via Corrosion"`

---

## Task B3: `nav` Rust crate (extract from src/nav.rs) via Corrosion

**Files:**
- Create: `src/crates/nav/{Cargo.toml,src/lib.rs}` (move `src/nav.rs` content), keep `include`? (the `GamePathfinder` struct is defined C-side; the crate only exports the thunks)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces (C ABI, unchanged from today): `nav_context_new()`, `nav_add_obstacle`, `nav_remove_obstacle`, `nav_find_path` — the exact signatures in the current `src/nav.rs` + `src/game_ffi.rs::GamePathfinder`. Consumed later by the Stage-2 game layer, not by Stage 1's app.

- [ ] **Step 1:** Create the crate; move `src/nav.rs` into `src/crates/nav/src/lib.rs`; Cargo.toml deps `cavalier_contours`, `parry2d`, `pathfinding`, `glam`; staticlib+rlib. Add a `nav_context_new()/nav_context_free()` export (today the Rust side boxes `NavContext` in `Game::new`; the crate must expose construction for the C++ caller).
- [ ] **Step 2:** Keep the existing inline `#[cfg(test)]` nav tests; `cargo test -p nav` — Expected: PASS (the 6 pathfinding tests).
- [ ] **Step 3:** Corrosion-import + link `nav` into `badlands` (no C++ caller yet — link only, verify symbols resolve with a `--as-needed` off or a trivial reference).
- [ ] **Step 4: Commit** `git commit -m "crates: nav extracted behind GamePathfinder C ABI"`

*(noiser: add `corrosion_import_crate(MANIFEST_PATH third_party/noiser/noiser-vm/Cargo.toml)` + link, mirroring sampo; a one-line smoke that a noiser symbol resolves. Fold into this task's commit if trivial, else its own commit.)*

---

## Task C1: Port `core/` types (camera, math glue, GeometryType)

**Files:**
- Port: `../sampo/src/core/camera.hpp` → `src/engine/core/camera.hpp` (Camera + `UniformData`, reversed-Z; keep the 576-byte `UniformData` layout matching `shaders/common/frame.wesl`)
- Port: sampo's `GeometryType` (`rendering/components/geometry_type.hpp`) → `src/core/geometry_type.hpp`
- Port: any small math helpers camera depends on → `src/core/`

**Interfaces:**
- Produces: `badlands::Camera` (`GetView`/`GetProj`), `badlands::UniformData`, `badlands::GeometryType {kTexturedMesh,…}`.

- [ ] **Step 1:** Port the files (namespace/includes). Ensure glm is available (add glm via CMake — vendored `third_party/glm` or FetchContent).
- [ ] **Step 2:** Add a `static_assert(sizeof(UniformData) == 576)` (matches the Rust reference + `frame.wesl`).
- [ ] **Step 3: Build** — Expected: compiles.
- [ ] **Step 4: Commit** `git commit -m "engine/core: port camera + UniformData + GeometryType"`

---

## Task C2: Port the lower-level render API (pipeline gen + reflection + frame context + vertex layout)

**Files:**
- Port → `src/engine/rendering/`: `shader/gpu_pipeline_generator.{hpp,cpp}`, `shader/shader_reflection.{hpp,cpp}`, `context/frame_context.{hpp,cpp}`, `vertex_layout.{hpp,cpp}`
- Modify: `gpu_pipeline_generator` WESL compilation to call `badlands_wesl_compile` (the `wesl` crate) then hand WGSL to tint/Dawn.
- Copy: `shaders/common/frame.wesl`, `shaders/passes/tonemapping.wesl` into badlands `shaders/`.

**Interfaces:**
- Consumes: `badlands_wesl_compile` (Task B1).
- Produces: `badlands::GpuPipelineGenerator` (`GetPipeline`, `GetBindGroupLayout`, reflection accessors), `badlands::FrameContext` (frame uniform buffer + dynamic uniform buffer), `VertexLayout`/`GetVertexLayoutInfo`.

- [ ] **Step 1:** Port the four files (+ transitive support). Replace sampo's WESL step with `badlands_wesl_compile(shader_dir, module)`.
- [ ] **Step 2:** Confirm sampo's C++ reflection path (tint-based) is intact — it derives bind-group layouts from WGSL (the data-driven unroll). Bring along `shader_reflection`'s deps.
- [ ] **Step 3: Smoke test** — in a temporary test hook: `GpuPipelineGenerator::GetPipeline` for `passes/tonemapping` returns a valid `wgpu::RenderPipeline` (reflection produced a layout). Build + run.
Run: `./build/badlands` — Expected: log "tonemapping pipeline compiled", no Dawn validation errors.
- [ ] **Step 4: Commit** `git commit -m "engine: port pipeline generator + reflection + frame context + vertex layout"`

---

## Task D1: Port the material system

**Files:**
- Port → `src/engine/rendering/material/`: the full `../sampo/src/rendering/material/*` (Material, MeshRenderingMaterial, MaterialInstance, MaterialInstanceFactory, StandardMaterialFactory, MaterialRequirements, MaterialInstanceCache, rendering_material_instance, standard_rendering_material_instance, material_id).
- Copy: `shaders/material/textured_mesh.wesl`, `shaders/material/normalmapped.wesl`.
- Note: `NoiserMaterialScript` recipe path needs noiser — link it (Task B3) or stub the script-texture provider for Stage 1 (default-view + 1×1 defaults only).

**Interfaces:**
- Consumes: `GpuPipelineGenerator`, reflection, `FrameContext`.
- Produces: `BuildMaterialInstanceFactory(FactoryDescriptor)` and `StandardMaterialFactory::CreateInstance(geometry, pass, InstanceParams)` → a `RenderingMaterialInstance` with `Bind`/`BindPerObject`.

- [ ] **Step 1:** Port the files; for Stage 1, stub `ScriptTextureProvider` to error (no `NoiserMaterialScript` recipes used yet) so noiser isn't a hard dep of the material path.
- [ ] **Step 2:** Bring the 1×1 default textures (white/flat_normal/full_roughness/gray) + `MaterialRequirementsRegistry` for `textured_mesh` + `normalmapped`.
- [ ] **Step 3: Smoke** — build a `textured_mesh` factory + `CreateInstance` with a 1×1 white default; assert the instance `IsValid()`. Build + run.
- [ ] **Step 4: Commit** `git commit -m "engine: port data-driven material system"`

---

## Task D2: Port the scene graph + mesh components + sphere builder

**Files:**
- Port → `src/engine/`: `scene/{scene_graph,scene_node,scene_attachment}.{hpp,cpp}`, `rendering/components/{mesh_components,transform,material_factory_component,forward_component}.hpp`, `rendering/geometry/textured_mesh_builders.{hpp,cpp}`.

**Interfaces:**
- Produces: `badlands::SceneGraph` (`CreateNode`, `SetLocalTransform`, `AddAttachment<MeshAttachment>`, `SetSunDirection/Color`, `SyncToRegistry`), `MeshAttachment{ResolvedMesh, factory, pass_type, params}`, `GenerateSphereTexturedMesh(radius, resolution)`.

- [ ] **Step 1:** Port the files (+ the EnTT registry usage; EnTT is already vendored under `third_party/entt`). Add EnTT to the CMake include path.
- [ ] **Step 2: Smoke** — `GenerateSphereTexturedMesh(1.0, 32)` returns a mesh with `vertex_count > 0` and `11` floats/vertex; a `SceneGraph` node with a `MeshAttachment` syncs into a registry with one renderable entity. Build + run.
- [ ] **Step 3: Commit** `git commit -m "engine: port scene graph + mesh components + sphere builder"`

---

## Task D3: Port the forward renderer; render an untextured sphere (build_test_scene seam)

**Files:**
- Port → `src/engine/rendering/`: `scene_renderer.{hpp,cpp}` (**forward-opaque + tonemap subset only** — omit gbuffer/deferred-lighting/shadow/GTAO/skybox/water/bloom passes and their members), `passes/render_textured_mesh.{hpp,cpp}`.
- Modify: `src/main.cpp` — add `build_test_scene(SceneRenderer&/SceneGraph&)`, the render loop, a fixed camera.

**Interfaces:**
- Consumes: everything above.
- Produces: `badlands::SceneRenderer::Render(const Camera&, const SceneContext&)`; `build_test_scene()` (the swap seam).

- [ ] **Step 1:** Port `scene_renderer` trimmed to the forward path: init HDR target + depth (reversed-Z), the textured-mesh forward pass, then the tonemapping node → surface. Delete/ifdef-out the deferred/shadow/post-fx passes and their setup.
- [ ] **Step 2:** Write `build_test_scene` in `main.cpp`:
```cpp
// The ONLY game-specific seam. Stage 2 replaces this body with world->scene.
static void build_test_scene(badlands::SceneGraph& scene,
                             badlands::StandardMaterialFactory& mat) {
  auto mesh = badlands::GenerateSphereTexturedMesh(1.0f, 48);
  auto node = scene.CreateNode("test_sphere", scene.Root());
  scene.AddAttachment(node, badlands::MeshAttachment{
      .mesh = std::move(mesh.mesh), .factory = &mat,
      .pass_type = badlands::MaterialPassType::kForwardOpaque, .params = {}});
  scene.SetSunDirection(glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)));
  scene.SetSunColor(glm::vec3(1.0f));
}
```
- [ ] **Step 3:** Main loop: fixed `Camera` looking at origin; each frame `scene.SyncToRegistry(...)`, `renderer.Render(camera, scene_context)`, present.
- [ ] **Step 4: Build + run + verify**
Run: `./build/badlands` — Expected: a lit, flat-shaded/1×1-white sphere renders (forward + tonemap), no Dawn validation errors.
- [ ] **Step 5: Commit** `git commit -m "engine: forward renderer + build_test_scene renders a sphere"`

---

## Task E1: Texture upload + GPU mip generation

**Files:**
- Port → `src/engine/`: `image_processing/tasks/mipmap_generation_task.{hpp,cpp}` (+ its `ResizeNode`/processing-graph deps trimmed to what mips need), `shaders/compute/mip_generator.wesl`, `shaders/compute/mip_generator_render.wesl`.
- Create: `src/engine/rendering/texture_loader.{hpp,cpp}` — `badlands::LoadTexture2D(device, queue, path)`: call `badlands_decode_jpeg` (assets crate) → create a Dawn texture with `mip_level_count = floor(log2(max(w,h)))+1`, upload level 0, run the mip-generation task, return a `wgpu::TextureView`.

**Interfaces:**
- Consumes: `badlands_decode_jpeg` (B2), the mip task.
- Produces: `badlands::LoadTexture2D(...) -> wgpu::TextureView` (mip-complete).

- [ ] **Step 1:** Port the mip task + shaders; adapt to badlands' `GpuPipelineGenerator`.
- [ ] **Step 2:** Implement `texture_loader` (decode → upload L0 → GPU mips).
- [ ] **Step 3: Smoke** — `LoadTexture2D(rocky_trail_diff)` returns a valid view; query the texture's mip count == 11 for 1024². No validation errors. Build + run.
- [ ] **Step 4: Commit** `git commit -m "engine: JPEG->Dawn texture + GPU mip generation"`

---

## Task E2: Textured, mipmapped, lit sphere (milestone)

**Files:**
- Modify: `src/main.cpp` — `build_test_scene` uses a `normalmapped` (or `textured_mesh`) material instance whose albedo is `LoadTexture2D("assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg")`.

**Interfaces:**
- Consumes: `LoadTexture2D`, `StandardMaterialFactory::CreateInstance` with `InstanceParams.texture_overrides`.

- [ ] **Step 1:** Build a `textured_mesh`/`normalmapped` factory; create an instance with the decoded albedo view in `texture_overrides` (slot `albedo`).
- [ ] **Step 2:** Point the test sphere's `MeshAttachment.factory` + `params` at it.
- [ ] **Step 3: Build + run + verify the milestone**
Run: `./build/badlands` — Expected: a lit sphere textured with the rocky_trail albedo, **mip-smooth** at grazing angles (no shimmer), no Dawn validation errors. Capture a screenshot if a readback path is wired; otherwise a windowed visual check.
- [ ] **Step 4: Commit** `git commit -m "engine: textured mipmapped sphere — Stage 1 milestone"`

---

## Self-Review notes (coverage vs spec)
- Platform (SDL3+Dawn+CMake): A1–A2. Rust libs via Corrosion (noiser/nav/wesl/assets): B1–B3. Lower-level API: C1–C2. Material system: D1. Scene graph + mesh: D2. Forward pass + tonemap + swap seam: D3. GPU mip generation + textures: E1–E2. → every Stage-1 spec bullet maps to a task.
- Deferred/GTAO/shadows/bloom/water/skybox: intentionally **out of scope** (spec: post-fx is Stage 5).
- `game/` untouched (spec) — no task modifies it.

## Risks specific to execution
- **Dawn first build** is long; budget for it in Task A2. Ninja + a warm FetchContent cache help.
- **tint vs naga reflection:** sampo's `shader_reflection` is tint-based; ensure the ported reflection compiles against the Dawn version pinned above (the reflection API tracks Dawn/tint versions).
- **Trimming `scene_renderer`:** it has many members for passes we omit; delete their init + usage cleanly (D3) rather than leaving dangling references.
- **Rust panic across FFI:** Corrosion sets `-Cpanic=unwind`; keep `catch_unwind` in every `extern "C"` thunk (wesl/assets), matching the current `nav.rs` thunks.
