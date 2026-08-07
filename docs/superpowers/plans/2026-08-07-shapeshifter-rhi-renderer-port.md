# Shapeshifter RHI Renderer Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `editors/shapeshifter`'s renderer off vendored metal-cpp and onto `badlands_rhi` + `badlands_render_graph`, so the editor and the game share one rendering substrate.

**Architecture:** A new `RhiRenderer` is built *alongside* the existing `Renderer`, unit-tested against the Null backend, and swapped in by one flip commit that then deletes the old path. The frame becomes three graph passes (`geometry` → `ground` → `chrome`) over seven pipelines; `rhi::ISwapchain` takes the `CAMetalLayer` Swift already hands across, and `CAMetalDisplayLink` demotes to a tick source.

**Tech Stack:** C++23, `badlands_rhi` (Metal + Null), `badlands_render_graph`, `badlands_slang` (Slang → MSL + reflection), Catch2 for the new GPU-side tests, doctest for the existing core suite, XcodeGen for the app.

## Global Constraints

- **Reversed-Z is an invariant.** Depth clears to `0.0` (far); "closer" compares `Greater`. `camera.cpp`'s projection maps near→1, far→0. Do not reintroduce `Less` or a `1.0` clear.
- **The RHI must never link SDL.** Shapeshifter links `badlands_rhi`, `badlands_render_graph`, `badlands_slang` — never `badlands_rhi_app`, which is SDL-bound and useless to a SwiftUI app.
- **The Slang SDK is a required prerequisite** (`scripts/fetch_slang.sh`, pinned v2026.14.1). Configure already fails without it.
- **Shader binding slots are the declaration order of the shader's globals**, which is what Slang reflection assigns. Name them as constants; do not hardcode integers at call sites.
- **Binding-table dynamic offsets are passed in INCREASING SLOT ORDER.** Reversing them applies each offset to the wrong binding and produces geometry from uniform bytes.
- **A binding table is immutable.** If the frame allocator's ring grows onto a different buffer, the table must be rebuilt — see `LinePass::Upload`.
- **Buffers a pass reads must be `graph.ImportBuffer(...)` + `pass.Reads(...)`**, or they are never transitioned and the validation layer reports them `Undefined`.
- **`shapeshifter_core` builds at `-mmacosx-version-min=15.0`** to match the app. Do not remove that.
- Build with `scripts/build.sh [target]`; test with `scripts/test.sh [regex]`. Run from the repo root.

## Reference implementation

`src/executables/object_viewer/line_pass.cpp` is the complete worked example of shader load → pipeline → frame allocator → binding table → graph pass. Read it before Task 2. `src/executables/object_viewer/main.cpp:240-290` is the frame shape.

---

## File Structure

| File | Responsibility |
|---|---|
| `editors/shapeshifter/core/src/rhi_pipelines.{h,cpp}` | **Create.** Loads the four Slang modules and builds the seven pipelines. Nothing per-frame. |
| `editors/shapeshifter/core/src/rhi_renderer.{h,cpp}` | **Create.** Per-frame: allocator, binding tables, uniform upload, the three graph passes, swapchain acquire/present. |
| `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp` | **Create.** Catch2, Null backend. Pipelines build; the graph declares the right passes, depth states and draws. |
| `editors/shapeshifter/core/src/editor.cpp` | **Modify.** Swap `Renderer` for `RhiRenderer`; `render()` loses its drawable parameter. |
| `editors/shapeshifter/core/include/shapeshifter/ShapeshifterCore.h` | **Modify.** `void render(void*)` → `void render()`. |
| `editors/shapeshifter/app/Sources/MetalViewport.swift` | **Modify.** Display link becomes a tick; stop passing the drawable. |
| `editors/shapeshifter/core/src/renderer.{h,cpp}` | **Delete** (Task 7). |
| `editors/shapeshifter/core/src/metal_impl.cpp`, `vendor/metal-cpp/` | **Delete** (Task 7). |
| `editors/shapeshifter/shaders/*.metal` | **Delete** (Task 7). |
| `editors/shapeshifter/shaders/{sdf_scene,shared_types,ground_grid}.h` | **Modify** (Task 7). Strip the `__METAL_VERSION__` arms. |
| `CMakeLists.txt` | **Modify.** Link the RHI; add the test target; drop metal-cpp include paths. |
| `editors/shapeshifter/project.yml` | **Modify** (Task 7). Drop the `.metal` build phase. |

**The seven pipelines** (state that Metal set per-draw is baked into the pipeline here):

| Name | Topology | Depth | Blend |
|---|---|---|---|
| `raymarch` | TriangleList | Greater, write | none |
| `mesh` | TriangleList | Greater, write | none |
| `ground` | TriangleList | Greater, no write | premultiplied |
| `origin` | TriangleList | Greater, no write | straight alpha |
| `lines` | LineList | off | none |
| `blend_lines` | LineList | off | straight alpha |
| `blend_tris` | TriangleList | off | straight alpha |

**The three graph passes:** `geometry` (colour clear + `DepthTarget` clear) → `ground` (colour load + `DepthReadOnly`) → `chrome` (colour load, **no depth attachment**; its pipelines declare no depth format, which is what satisfies Metal's format-match validation).

---

### Task 1: Link the RHI into `shapeshifter_core`, with a smoke test

**Files:**
- Modify: `CMakeLists.txt` (the `shapeshifter_core` block, ~line 2099)
- Create: `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp`

**Interfaces:**
- Consumes: `badlands::rhi::CreateDevice`, `badlands::rhi::BackendKind`
- Produces: target `shapeshifter_rhi_tests`; `shapeshifter_core` now links `badlands_rhi badlands_render_graph badlands_slang`

- [ ] **Step 1: Write the failing test**

Create `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp`:

```cpp
// The editor's RHI renderer, on the Null backend where possible.
//
// Catch2 rather than the doctest the rest of editors/shapeshifter uses: this
// target links badlands_rhi and the graph, whose own suites are Catch2, and a
// second framework in one binary is worse than a second framework in the repo.

#include <catch_amalgamated.hpp>

#include "engine/rhi/rhi_device.hpp"

using namespace badlands::rhi;

TEST_CASE("shapeshifter: the core can create an RHI device", "[ss-rhi]") {
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true,
                              .label = "shapeshifter_tests"});
  REQUIRE(device != nullptr);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `scripts/build.sh shapeshifter_rhi_tests`
Expected: FAIL — no such target.

- [ ] **Step 3: Link the RHI and add the target**

In `CMakeLists.txt`, in the `shapeshifter_core` block, replace the "does NOT link badlands_rhi yet" comment and add the libraries:

```cmake
# The editor renders through the engine's RHI and render graph, and compiles its
# shaders with the engine's Slang layer. NOT badlands_rhi_app: that shell is
# SDL-bound, and this app's window is SwiftUI/AppKit.
target_link_libraries(shapeshifter_core PUBLIC
    badlands_rhi badlands_render_graph badlands_slang
    "-framework Metal" "-framework Foundation"
    "-framework QuartzCore" "-framework CoreGraphics")
```

Then append the test target next to `shapeshifter_core_tests`:

```cmake
# shapeshifter_rhi_tests: the RHI-side renderer, on Null. Catch2 because it
# links the engine's suites' framework; the doctest suite next door is the
# editor's own pure-CPU core and stays as it is.
add_executable(shapeshifter_rhi_tests
    ${SHAPESHIFTER_DIR}/tests/core/rhi_renderer_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(shapeshifter_rhi_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(shapeshifter_rhi_tests PRIVATE shapeshifter_core)
if(APPLE)
    target_link_options(shapeshifter_rhi_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME shapeshifter_rhi_tests COMMAND shapeshifter_rhi_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 4: Run it to verify it passes**

Run: `scripts/build.sh shapeshifter_rhi_tests && scripts/test.sh shapeshifter_rhi_tests`
Expected: PASS, 1 test case.

Also run: `scripts/test.sh shapeshifter_core_tests`
Expected: still 223 cases. Linking the RHI must not disturb the existing suite.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt editors/shapeshifter/tests/core/rhi_renderer_tests.cpp
git commit -m "build(shapeshifter): link the RHI, graph and Slang into the core"
```

---

### Task 2: The seven pipelines

**Files:**
- Create: `editors/shapeshifter/core/src/rhi_pipelines.h`, `editors/shapeshifter/core/src/rhi_pipelines.cpp`
- Modify: `CMakeLists.txt` (add the .cpp to `shapeshifter_core`)
- Test: `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp`

**Interfaces:**
- Consumes: `slang::CreateSlangCompiler`, `IRhiDevice::CreateShaderModule`, `IRhiDevice::CreateRenderPipeline`
- Produces: `sq::RhiPipelines` with `static std::unique_ptr<RhiPipelines> Create(rhi::IRhiDevice&, slang::SlangCompiler&, rhi::Format color, rhi::Format depth)` and public members `raymarch, mesh, ground, origin, lines, blend_lines, blend_tris` (each `rhi::RenderPipelinePtr`)

- [ ] **Step 1: Write the failing test**

Append to `rhi_renderer_tests.cpp`:

```cpp
#include "rhi_pipelines.h"
#include "engine/slang/slang_compiler.hpp"

namespace {
std::unique_ptr<badlands::slang::SlangCompiler> MakeCompiler() {
  const std::string paths[] = {"editors/shapeshifter/shaders/slang/shapeshifter",
                               "editors/shapeshifter/shaders"};
  return badlands::slang::CreateSlangCompiler(paths);
}
}  // namespace

TEST_CASE("shapeshifter: all seven pipelines build", "[ss-rhi]") {
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "pipelines"});
  REQUIRE(device);
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto p = sq::RhiPipelines::Create(*device, *compiler, Format::RGBA16Float,
                                    Format::Depth32Float);
  REQUIRE(p != nullptr);
  CHECK(p->raymarch);
  CHECK(p->mesh);
  CHECK(p->ground);
  CHECK(p->origin);
  CHECK(p->lines);
  CHECK(p->blend_lines);
  CHECK(p->blend_tris);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `scripts/build.sh shapeshifter_rhi_tests`
Expected: FAIL — `rhi_pipelines.h` not found.

- [ ] **Step 3: Write the header**

Create `editors/shapeshifter/core/src/rhi_pipelines.h`:

```cpp
#pragma once

// The editor's render pipelines, built once.
//
// SEVEN, where the metal-cpp path had five PSOs. Metal let a draw pick its
// depth-stencil state and primitive type independently; the RHI folds both into
// the pipeline, so `line_blend_pso_` alone becomes three -- lines and triangles,
// depth-tested and depth-ignored.

#include <memory>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace sq {

struct RhiPipelines {
  // `depth` is the format of the depth attachment the geometry and ground
  // passes render into. The chrome pipelines declare NO depth format: their
  // pass has no depth attachment, and Metal validation requires a pipeline's
  // depth format to match the pass it is used in.
  static std::unique_ptr<RhiPipelines> Create(badlands::rhi::IRhiDevice& device,
                                              badlands::slang::SlangCompiler& compiler,
                                              badlands::rhi::Format color,
                                              badlands::rhi::Format depth);

  badlands::rhi::RenderPipelinePtr raymarch;     // depth write, opaque
  badlands::rhi::RenderPipelinePtr mesh;         // depth write, opaque
  badlands::rhi::RenderPipelinePtr ground;       // depth read, premultiplied
  badlands::rhi::RenderPipelinePtr origin;       // depth read, straight alpha
  badlands::rhi::RenderPipelinePtr lines;        // no depth, opaque, LINES
  badlands::rhi::RenderPipelinePtr blend_lines;  // no depth, alpha, LINES
  badlands::rhi::RenderPipelinePtr blend_tris;   // no depth, alpha, TRIANGLES

 private:
  badlands::rhi::ShaderModulePtr modules_[8];
};

}  // namespace sq
```

- [ ] **Step 4: Write the implementation**

Create `editors/shapeshifter/core/src/rhi_pipelines.cpp`:

```cpp
#include "rhi_pipelines.h"

#include <spdlog/spdlog.h>

namespace sq {

using namespace badlands::rhi;
namespace slang = badlands::slang;

namespace {

// Straight alpha: the gizmo chrome and the origin marker are ordinary coloured
// vertices, not composited coverage. This is NOT rhi::AlphaBlend(), whose alpha
// source factor is One.
constexpr BlendState kStraightAlpha{
    .enabled = true,
    .color = {.src = BlendFactor::SrcAlpha,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add},
    .alpha = {.src = BlendFactor::SrcAlpha,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add}};

// Premultiplied: the ground plate's shader returns coverage already multiplied
// through, so the source factor is One.
constexpr BlendState kPremultiplied{
    .enabled = true,
    .color = {.src = BlendFactor::One,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add},
    .alpha = {.src = BlendFactor::One,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add}};

// Greater, not GreaterEqual: the exact mirror of the Less this replaced, so the
// port changes precision and leaves equal-depth behaviour a rejection.
DepthState DepthWrite(Format depth) {
  return {.test_enabled = true, .write_enabled = true,
          .compare = CompareFunction::Greater, .format = depth};
}
DepthState DepthRead(Format depth) {
  return {.test_enabled = true, .write_enabled = false,
          .compare = CompareFunction::Greater, .format = depth};
}
DepthState NoDepth() { return {}; }  // test off, write off, format Undefined

}  // namespace

std::unique_ptr<RhiPipelines> RhiPipelines::Create(
    IRhiDevice& device, slang::SlangCompiler& compiler, Format color,
    Format depth) {
  auto p = std::make_unique<RhiPipelines>();
  int next = 0;

  auto load = [&](const char* module, const char* entry) -> IShaderModule* {
    auto compiled = compiler.Get({.module = module, .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    auto m = device.CreateShaderModule(compiled->source, compiled->reflection,
                                       std::string(module) + "::" + entry);
    if (!m) return nullptr;
    p->modules_[next] = std::move(m);
    return p->modules_[next++].get();
  };

  auto* rm_vs = load("raymarch", "vs_main");
  auto* rm_fs = load("raymarch", "fs_main");
  auto* gg_vs = load("ground_grid", "vs_main");
  auto* gg_fs = load("ground_grid", "fs_main");
  auto* ms_vs = load("mesh", "vs_main");
  auto* ms_fs = load("mesh", "fs_main");
  auto* ln_vs = load("debug_lines", "vs_main");
  auto* ln_fs = load("debug_lines", "fs_main");
  if (!rm_vs || !rm_fs || !gg_vs || !gg_fs || !ms_vs || !ms_fs || !ln_vs ||
      !ln_fs) {
    spdlog::error("shapeshifter: a shader module failed to build");
    return nullptr;
  }

  auto make = [&](IShaderModule* vs, IShaderModule* fs, DepthState d,
                  PrimitiveTopology topo, std::vector<BlendState> blend,
                  const char* label) {
    return device.CreateRenderPipeline({.vertex_shader = vs,
                                        .vertex_entry = "vs_main",
                                        .fragment_shader = fs,
                                        .fragment_entry = "fs_main",
                                        .color_formats = {color},
                                        .blend_states = std::move(blend),
                                        .depth = d,
                                        .topology = topo,
                                        .cull_mode = CullMode::None,
                                        .label = label});
  };

  p->raymarch = make(rm_vs, rm_fs, DepthWrite(depth),
                     PrimitiveTopology::TriangleList, {}, "raymarch");
  p->mesh = make(ms_vs, ms_fs, DepthWrite(depth),
                 PrimitiveTopology::TriangleList, {}, "mesh");
  p->ground = make(gg_vs, gg_fs, DepthRead(depth),
                   PrimitiveTopology::TriangleList, {kPremultiplied}, "ground");
  p->origin = make(ln_vs, ln_fs, DepthRead(depth),
                   PrimitiveTopology::TriangleList, {kStraightAlpha}, "origin");
  p->lines = make(ln_vs, ln_fs, NoDepth(), PrimitiveTopology::LineList, {},
                  "lines");
  p->blend_lines = make(ln_vs, ln_fs, NoDepth(), PrimitiveTopology::LineList,
                        {kStraightAlpha}, "blend_lines");
  p->blend_tris = make(ln_vs, ln_fs, NoDepth(),
                       PrimitiveTopology::TriangleList, {kStraightAlpha},
                       "blend_tris");

  if (!p->raymarch || !p->mesh || !p->ground || !p->origin || !p->lines ||
      !p->blend_lines || !p->blend_tris) {
    spdlog::error("shapeshifter: a render pipeline failed to build");
    return nullptr;
  }
  return p;
}

}  // namespace sq
```

Add `${SHAPESHIFTER_DIR}/core/src/rhi_pipelines.cpp` to the `shapeshifter_core` source list.

- [ ] **Step 5: Run it to verify it passes**

Run: `scripts/build.sh shapeshifter_rhi_tests && scripts/test.sh shapeshifter_rhi_tests`
Expected: PASS, 2 test cases.

If `PrimitiveTopology::LineList` does not exist under that name, grep `rhi_types.hpp` for the enum and use the actual spelling — do not invent one.

- [ ] **Step 6: Commit**

```bash
git add editors/shapeshifter/core/src/rhi_pipelines.{h,cpp} \
        editors/shapeshifter/tests/core/rhi_renderer_tests.cpp CMakeLists.txt
git commit -m "feat(shapeshifter): the editor's seven RHI pipelines"
```

---

### Task 3: `RhiRenderer` — allocator, tables, and the three graph passes

**Files:**
- Create: `editors/shapeshifter/core/src/rhi_renderer.h`, `editors/shapeshifter/core/src/rhi_renderer.cpp`
- Modify: `CMakeLists.txt`
- Test: `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp`

**Interfaces:**
- Consumes: `sq::RhiPipelines`, `rhi::FrameAllocator`, `graph::RenderGraph`
- Produces: `sq::RhiRenderer` with `bool BuildFrame(graph::RenderGraph&, graph::ResourceHandle color, graph::ResourceHandle depth, const SceneDocument&, int32_t selected, const Camera&, uint32_t w, uint32_t h)`

This is the substance. Model it on `LinePass::Upload` + `AddToGraph`. The uniform and vertex data both come from one `FrameAllocator` (`BufferUsage::Uniform | BufferUsage::Storage`), and the binding table is rebuilt whenever an allocation lands on a different buffer than the table was built against.

- [ ] **Step 1: Write the failing test**

```cpp
#include "rhi_renderer.h"
#include "engine/graph/render_graph.hpp"
#include "engine/rhi/null/null_rhi.hpp"
#include "scene.h"
#include "camera.h"

namespace null = badlands::rhi::null;
namespace graph = badlands::graph;

TEST_CASE("shapeshifter: the frame is three passes, and only the first two "
          "touch depth", "[ss-rhi]") {
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "frame"});
  REQUIRE(device);
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  auto renderer = sq::RhiRenderer::Create(*device, *compiler,
                                          Format::RGBA16Float);
  REQUIRE(renderer);

  auto color_tex = device->CreateTexture(
      {.width = 64, .height = 64, .format = Format::RGBA16Float,
       .usage = TextureUsage::RenderTarget, .label = "color"});
  auto depth_tex = device->CreateTexture(
      {.width = 64, .height = 64, .format = Format::Depth32Float,
       .usage = TextureUsage::DepthStencil, .label = "depth"});
  REQUIRE(color_tex);
  REQUIRE(depth_tex);

  // `add` is the direct test entry point; spawn_snapped/spawn_unsnapped are the
  // real ones and do id/name allocation this test does not need.
  sq::SceneDocument doc;
  sq::Node node;
  node.id = 1;
  node.shape = sq::Shape::Cube;
  doc.add(node);
  sq::Camera camera;
  camera.aspect = 1.0f;

  device->BeginFrame();
  renderer->BeginFrame(device->CurrentFrame());

  graph::RenderGraph g(*device);
  auto color = g.ImportTexture(color_tex.get(), ResourceState::Undefined, "c");
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined, "d");
  REQUIRE(renderer->BuildFrame(g, color, depth, doc, sq::kInvalidNode, camera,
                               64, 64));
  REQUIRE(g.Compile());

  auto* log = null::GetCommandLog(*device);
  REQUIRE(log);
  log->Clear();
  auto encoder = device->CreateCommandEncoder("frame");
  g.Execute(*encoder);
  encoder->Finish();
  device->Submit(*encoder);
  device->EndFrame();
  device->WaitIdle();

  REQUIRE(log->Count(null::RecordedCommand::Kind::BeginRenderPass) == 3);
  const auto* geometry = log->Find(null::RecordedCommand::Kind::BeginRenderPass, 0);
  const auto* ground   = log->Find(null::RecordedCommand::Kind::BeginRenderPass, 1);
  const auto* chrome   = log->Find(null::RecordedCommand::Kind::BeginRenderPass, 2);
  REQUIRE(geometry); REQUIRE(ground); REQUIRE(chrome);

  // Geometry clears depth to the reversed-Z far value and writes it.
  CHECK(geometry->has_depth);
  CHECK(geometry->depth_load == LoadOp::Clear);
  CHECK_FALSE(geometry->depth_read_only);
  // The ground plate tests against what geometry wrote, and preserves it.
  CHECK(ground->has_depth);
  CHECK(ground->depth_load == LoadOp::Load);
  CHECK(ground->depth_read_only);
  CHECK(ground->depth_store == StoreOp::Store);
  // The chrome pass is depth-ignored, so it has no depth attachment at all --
  // which is what lets its pipelines declare no depth format.
  CHECK_FALSE(chrome->has_depth);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `scripts/build.sh shapeshifter_rhi_tests`
Expected: FAIL — `rhi_renderer.h` not found.

- [ ] **Step 3: Implement `RhiRenderer`**

Write `rhi_renderer.{h,cpp}` following `line_pass.cpp` exactly for the allocator/table/rebuild dance. Structure of `BuildFrame`:

```cpp
// geometry: clears colour and depth, writes both.
auto geometry = graph.AddRasterPass("geometry");
geometry.ColorTarget(color, LoadOp::Clear, StoreOp::Store, kClearColor);
geometry.DepthTarget(depth, LoadOp::Clear, StoreOp::Store, 0.0f);  // reversed-Z far
geometry.Reads(ring);            // the imported frame-allocator buffer
geometry.Execute([this](const graph::RasterContext& ctx) {
  if (node_count_ > 0) {
    const uint32_t offsets[2] = {raymarch_uniform_offset_, node_offset_};
    ctx.pass->SetPipeline(pipelines_->raymarch.get());
    ctx.pass->SetBindingTable(0, raymarch_table_.get(), offsets);
    ctx.pass->Draw(3);           // fullscreen triangle
  }
  if (mesh_vertex_count_ > 0) { /* ... mesh ... */ }
});

// ground: loads colour, tests depth without writing.
auto ground = graph.AddRasterPass("ground");
ground.ColorTarget(color, LoadOp::Load, StoreOp::Store);
ground.DepthReadOnly(depth);
ground.Reads(ring);
ground.Execute(/* ground plate, then the origin marker */);

// chrome: loads colour, NO depth attachment.
auto chrome = graph.AddRasterPass("chrome");
chrome.ColorTarget(color, LoadOp::Load, StoreOp::Store);
chrome.Reads(ring);
chrome.Execute(/* scene lines, gizmo grid, handles, focus dot, pivot */);
```

Draw order inside `chrome` is exactly the metal-cpp order — scene lines, gizmo grid, gizmo handles, focus preview, pivot marker — because it is painter's order and the pivot must be last.

- [ ] **Step 4: Run it to verify it passes**

Run: `scripts/build.sh shapeshifter_rhi_tests && scripts/test.sh shapeshifter_rhi_tests`
Expected: PASS. If validation reports a resource `Undefined`, a buffer was used without `ImportBuffer` + `Reads`.

- [ ] **Step 5: Commit**

```bash
git add editors/shapeshifter/core/src/rhi_renderer.{h,cpp} \
        editors/shapeshifter/tests/core/rhi_renderer_tests.cpp CMakeLists.txt
git commit -m "feat(shapeshifter): the frame as three graph passes"
```

---

### Task 4: Swapchain over the Swift-owned `CAMetalLayer`

**Files:**
- Modify: `editors/shapeshifter/core/src/rhi_renderer.{h,cpp}`
- Test: `editors/shapeshifter/tests/core/rhi_renderer_tests.cpp`

**Interfaces:**
- Produces: `RhiRenderer::AttachLayer(void* ca_metal_layer, uint32_t w, uint32_t h)`, `RhiRenderer::Resize(uint32_t, uint32_t)`, `RhiRenderer::RenderFrame(const SceneDocument&, int32_t, const Camera&)`

**`Editor` owns the device and the compiler, not `RhiRenderer`.** Task 3 gave `RhiRenderer::Create(IRhiDevice&, SlangCompiler&, Format)` so a test can hand it a Null device; `AttachLayer` must not also create one, or the two construction paths diverge. `Editor::attachLayer` therefore does: create the Metal device (once, app lifetime), create the compiler, `RhiRenderer::Create(...)`, then `renderer_->AttachLayer(layer, w, h)` which only builds the swapchain.

`SwapchainDesc::native_window` takes the `CAMetalLayer*` — the same pointer `Editor::attachLayer` already receives from Swift, so the interop surface does not change here.

- [ ] **Step 1: Write the failing test**

Null's swapchain supports fault injection, so acquire-skip is testable with no display:

```cpp
TEST_CASE("shapeshifter: a skipped acquire renders nothing and presents "
          "nothing", "[ss-rhi]") {
  // A minimized or occluded window makes Acquire return Skip every frame. The
  // renderer must treat that as "try again next frame", not as failure, and
  // must not present -- which is what PresentCount proves.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "skip"});
  REQUIRE(device);
  auto sc = device->CreateSwapchain({.native_window = nullptr,  // headless
                                     .width = 64, .height = 64,
                                     .format = Format::RGBA16Float,
                                     .label = "sc"});
  REQUIRE(sc);
  null::SetSwapchainFault(*sc, null::SwapchainFault::Skip);

  const uint64_t before = null::PresentCount(*sc);
  auto frame = sc->Acquire();
  CHECK(frame.status == AcquireStatus::Skip);
  CHECK(frame.view == nullptr);
  CHECK(null::PresentCount(*sc) == before);
}
```

- [ ] **Step 2: Run it to verify it fails, then implement**

Run: `scripts/build.sh shapeshifter_rhi_tests`

Implement `AttachLayer` (build the swapchain only — the device, compiler and pipelines already exist from `Create`), `Resize` (call `swapchain_->Resize` at ONE point in the frame — after `BeginFrame`, before `Acquire`), and `RenderFrame`:

```cpp
device_->BeginFrame();
BeginFrame(device_->CurrentFrame());     // recycles the allocator slot
// AFTER BeginFrame, so a SKIPPED frame still recycles its slot -- doing this
// after the Acquire check leaks slots on a minimized window.
if (pending_resize_) { swapchain_->Resize(width_, height_); pending_resize_ = false; }
auto frame = swapchain_->Acquire();
if (frame.status != AcquireStatus::Ok) { device_->EndFrame(); return; }

graph::RenderGraph g(*device_);
auto color = g.ImportTexture(frame.view->GetTexture(), ResourceState::Undefined, "backbuffer");
auto depth = g.CreateTexture({.width = width_, .height = height_,
                              .format = Format::Depth32Float,
                              .usage = TextureUsage::DepthStencil,
                              .label = "depth"});
if (!BuildFrame(g, color, depth, doc, selected, camera, width_, height_)) { device_->EndFrame(); return; }
if (!g.Compile()) { device_->EndFrame(); return; }
auto encoder = device_->CreateCommandEncoder("frame");
g.Execute(*encoder);
encoder->Finish();
device_->Submit(*encoder);
swapchain_->Present();
device_->EndFrame();
```

The graph is rebuilt per frame because the drawable is a different texture each time — caching a graph keyed on a resource that changes every frame is how a stale view gets rendered into.

- [ ] **Step 3: Run the suite**

Run: `scripts/test.sh shapeshifter_rhi_tests`

- [ ] **Step 4: Commit**

```bash
git add editors/shapeshifter/core/src/rhi_renderer.{h,cpp} \
        editors/shapeshifter/tests/core/rhi_renderer_tests.cpp
git commit -m "feat(shapeshifter): swapchain over the view's CAMetalLayer"
```

---

### Task 5: The flip

**Files:**
- Modify: `editors/shapeshifter/core/src/editor.cpp`
- Modify: `editors/shapeshifter/core/include/shapeshifter/ShapeshifterCore.h:133`
- Modify: `editors/shapeshifter/app/Sources/MetalViewport.swift:147-150`

**Interfaces:**
- Changes: `Editor::render(void* caMetalDrawable)` → `Editor::render()`

This is the irreducible commit: after it the editor renders through the RHI, and there is no intermediate state where both paths run.

- [ ] **Step 1: Change the interop signature**

In `ShapeshifterCore.h`:

```cpp
    // The RHI's swapchain acquires the drawable itself, so the caller supplies
    // none. CAMetalDisplayLink is now only a tick: it still paces the frame,
    // but its Update.drawable is unused.
    void render();
```

- [ ] **Step 2: Swap the renderer in `editor.cpp`**

Replace the `Renderer renderer_` member with `RhiRenderer`, `attachLayer` forwarding to `RhiRenderer::AttachLayer`, and `render()` calling `RenderFrame(doc_, selected_id_, camera_)`.

- [ ] **Step 3: Update Swift**

In `MetalViewport.swift`:

```swift
    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        // The drawable in `update` is deliberately unused: the RHI's swapchain
        // calls nextDrawable itself, so this callback is a tick. The
        // autoreleasepool stays -- the Metal backend is ARC Obj-C++.
        autoreleasepool {
            editor.render()
        }
    }
```

- [ ] **Step 4: Build and run the app**

Run:
```sh
scripts/build.sh shapeshifter_core
xcodegen generate --spec editors/shapeshifter/project.yml
xcodebuild -project editors/shapeshifter/Shapeshifter.xcodeproj -scheme Shapeshifter build \
           -derivedDataPath editors/shapeshifter/DerivedData
open editors/shapeshifter/DerivedData/Build/Products/Debug/Shapeshifter.app
```

**This is the checkpoint that matters.** Confirm: the ground grid renders; spawning each of the eight shapes renders it; the grid is occluded behind a shape rather than painted over it; the gizmo and pivot chrome draw on top; orbiting shows no z-fighting.

Then run it once under Metal's validation layers and confirm a clean log:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  editors/shapeshifter/DerivedData/Build/Products/Debug/Shapeshifter.app/Contents/MacOS/Shapeshifter
```

- [ ] **Step 5: Commit**

```bash
git add editors/shapeshifter/core editors/shapeshifter/app
git commit -m "feat(shapeshifter): render through the RHI"
```

---

### Task 6: Delete the metal-cpp path

**Files:**
- Delete: `editors/shapeshifter/core/src/renderer.{h,cpp}`, `core/src/metal_impl.cpp`, `vendor/metal-cpp/`, `shaders/*.metal`
- Modify: `shaders/{sdf_scene,shared_types,ground_grid}.h` — strip the `__METAL_VERSION__` arms
- Modify: `CMakeLists.txt`, `editors/shapeshifter/project.yml`

**Interfaces:** none — this removes code nothing calls.

- [ ] **Step 1: Delete, and drop the `.metal` build phase**

```bash
git rm -r editors/shapeshifter/core/src/renderer.h editors/shapeshifter/core/src/renderer.cpp \
          editors/shapeshifter/core/src/metal_impl.cpp editors/shapeshifter/vendor/metal-cpp \
          editors/shapeshifter/shaders/*.metal
```

In `project.yml`, remove the `shaders` entry from the app target's `sources` and `MTL_ENABLE_DEBUG_INFO`. In `CMakeLists.txt`, remove `metal_impl.cpp`, `renderer.cpp` and the `vendor/metal-cpp` include path.

- [ ] **Step 2: Strip the MSL arms from the three headers**

Each `#if defined(SDF_SLANG) / #elif defined(__METAL_VERSION__) / #else` collapses to `#if defined(SDF_SLANG) / #else`. Update each header's top comment: they are dual-compile again — C++ and Slang — not triple.

- [ ] **Step 3: Verify all three consumers still build**

Run:
```sh
scripts/build.sh && scripts/test.sh
xcodegen generate --spec editors/shapeshifter/project.yml
xcodebuild -project editors/shapeshifter/Shapeshifter.xcodeproj -scheme Shapeshifter build \
           -derivedDataPath editors/shapeshifter/DerivedData
```
Expected: `BUILD OK (all)`, every suite passing, app builds. Open it again and repeat the Task 5 visual check — deleting the MSL arm must not have changed the Slang arm, and this proves it.

- [ ] **Step 4: Update the docs**

`editors/shapeshifter/CLAUDE.md` says the core "still drives Metal directly through vendored metal-cpp" and calls the port "the next step". Rewrite that section: it drives the RHI, the shaders are Slang, and the header is dual-compile. `README.md`'s architecture section names `metal-cpp` too.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore(shapeshifter): delete the metal-cpp path"
```

---

## Verification

End to end, from the repo root:

```sh
scripts/build.sh                       # BUILD OK (all)
scripts/test.sh                        # every suite, including shapeshifter_rhi_tests
xcodegen generate --spec editors/shapeshifter/project.yml
xcodebuild -project editors/shapeshifter/Shapeshifter.xcodeproj -scheme Shapeshifter build \
           -derivedDataPath editors/shapeshifter/DerivedData
open editors/shapeshifter/DerivedData/Build/Products/Debug/Shapeshifter.app
```

The app is the only check for the parts no test reaches — blending, the swapchain, and depth ordering on a real GPU. Spawn all eight shapes, orbit, drag a gizmo, and confirm the ground plate occludes correctly. Run once under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` and confirm a clean log.

`grep -rn "metal-cpp\|MTL::\|\.metal" editors/shapeshifter --include=*.h --include=*.cpp --include=*.yml` should return nothing when this is done.
