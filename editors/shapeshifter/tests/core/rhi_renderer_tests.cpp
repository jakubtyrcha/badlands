// The editor's RHI renderer, on the Null backend wherever the property allows.
//
// CATCH2, not the doctest the rest of editors/shapeshifter uses. This target
// links badlands_rhi and the render graph, whose own suites are Catch2, and two
// frameworks in one binary is worse than two in one repo. The editor's pure-CPU
// core keeps its doctest suite next door, untouched.

#include <catch_amalgamated.hpp>

#include <string>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

#include "rhi_pipelines.h"

using namespace badlands::rhi;

namespace {

// Both paths: the entry points live in shaders/slang/shapeshifter, and the
// headers they include (shared_types.h, sdf_scene.h, ground_grid.h) live one
// level up in shaders/. Relative to the repo root, which is where ctest runs.
std::unique_ptr<badlands::slang::SlangCompiler> MakeCompiler() {
  const std::string paths[] = {
      "editors/shapeshifter/shaders/slang/shapeshifter",
      "editors/shapeshifter/shaders"};
  return badlands::slang::CreateSlangCompiler(paths);
}

}  // namespace

TEST_CASE("shapeshifter: the core can create an RHI device", "[ss-rhi]") {
  // The whole of task 1: proves shapeshifter_core links the RHI and that a
  // device can be made from inside the editor's own target, before anything is
  // built on top of it.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true,
                              .label = "shapeshifter_tests"});
  REQUIRE(device != nullptr);
}

TEST_CASE("shapeshifter: all seven pipelines build", "[ss-rhi]") {
  // SEVEN where the metal-cpp path had five PSOs. Metal let each draw pick its
  // depth-stencil state and primitive type; the RHI folds both into the
  // pipeline, so the one blend PSO becomes three -- lines and triangles,
  // depth-tested and depth-ignored.
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

TEST_CASE("shapeshifter: all seven pipelines build ON METAL", "[ss-rhi][metal]") {
  // The Null case above proves Slang compiled and the descriptors are
  // structurally valid. It cannot prove more: Null "runs no shaders" by its own
  // comment, so a pipeline it accepts may still be refused by a real backend --
  // an attachment format that cannot blend, a depth format the pass disagrees
  // with, an entry point name that does not exist in the emitted MSL. Those only
  // surface where the MSL is actually compiled.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true, .label = "pipelines"});
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }
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

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/null/null_rhi.hpp"
#include "rhi_renderer.h"
#include "camera.h"
#include "scene.h"

namespace null = badlands::rhi::null;
namespace graph = badlands::graph;

TEST_CASE("shapeshifter: the frame is three passes, and only the first two "
          "touch depth", "[ss-rhi]") {
  // The whole shape of the port in one assertion. Metal renders correctly
  // whether or not the depth declarations are right, so this is the only place
  // "geometry writes depth, the ground plate tests it without writing, the
  // chrome ignores it" is checkable at all.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "frame"});
  REQUIRE(device);
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  auto renderer = sq::RhiRenderer::Create(*device, *compiler, Format::RGBA16Float);
  REQUIRE(renderer);

  auto color_tex = device->CreateTexture(
      {.width = 64, .height = 64, .format = Format::RGBA16Float,
       .usage = TextureUsage::RenderTarget, .label = "color"});
  auto depth_tex = device->CreateTexture(
      {.width = 64, .height = 64, .format = Format::Depth32Float,
       .usage = TextureUsage::DepthStencil, .label = "depth"});
  REQUIRE(color_tex);
  REQUIRE(depth_tex);

  // `add` is the direct test entry point; spawn_snapped/spawn_unsnapped do id
  // and name allocation this does not need.
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
  // The ground plate tests what geometry wrote, and PRESERVES it -- discarding
  // here would leave the buffer undefined for anything reading it later.
  CHECK(ground->has_depth);
  CHECK(ground->depth_load == LoadOp::Load);
  CHECK(ground->depth_read_only);
  CHECK(ground->depth_store == StoreOp::Store);
  // The chrome pass has NO depth attachment, which is what lets its pipelines
  // declare no depth format and still satisfy Metal's format-match rule.
  CHECK_FALSE(chrome->has_depth);

  // The scene has one node, so the raymarch draws; the mesh is dormant, so it
  // does not. A frame that recorded no draws would satisfy every check above.
  CHECK(log->Count(null::RecordedCommand::Kind::Draw) >= 2);
}

TEST_CASE("shapeshifter: a skipped acquire presents nothing", "[ss-rhi]") {
  // A minimized or occluded window makes Acquire return Skip every frame. That
  // is a NORMAL frame, not a failure, and the renderer must not present -- which
  // is the part PresentCount can prove and a log cannot.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "skip"});
  REQUIRE(device);
  auto sc = device->CreateSwapchain({.native_window = nullptr, // headless
                                     .width = 64, .height = 64,
                                     .format = Format::RGBA16Float,
                                     .label = "sc"});
  REQUIRE(sc);
  null::SetSwapchainFault(*sc, null::SwapchainFault::Skip);

  const uint64_t before = null::PresentCount(*sc);
  device->BeginFrame();
  const auto frame = sc->Acquire();
  CHECK(frame.status == AcquireStatus::Skip);
  CHECK(frame.view == nullptr);
  device->EndFrame();
  CHECK(null::PresentCount(*sc) == before);
}

#include <algorithm>
#include <cstring>
#include <utility>

#include "badlands_assets.h"

TEST_CASE("shapeshifter: a headless frame draws the scene", "[ss-rhi][dump]") {
  // Renders one frame to a texture, asserts on the pixels, and writes a PNG
  // beside them.
  //
  // THIS IS THE TEST THAT WAS MISSING. Everything else passed while the app
  // rendered nothing: the pipelines built, the graph compiled, Metal's
  // validation layer stayed silent, and the window cleared to the background
  // colour and stopped. "The GPU did not complain" is not "the frame drew
  // something", and only reading the pixels back tells them apart.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true, .label = "dump"});
  if (!device) { SUCCEED("no Metal device"); return; }
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  const uint32_t W = 512, H = 512;
  // The APP renders at RGBA16Float; this dump started at RGBA8Unorm because it
  // reads back as bytes. Running both is what tells "the renderer is wrong"
  // apart from "the target format is".
  const auto [fmt, tag] = GENERATE(
      std::pair{Format::RGBA8Unorm, "rgba8"},
      std::pair{Format::RGBA16Float, "rgba16f"});
  CAPTURE(tag);
  auto renderer = sq::RhiRenderer::Create(*device, *compiler, fmt);
  REQUIRE(renderer);

  auto color_tex = device->CreateTexture(
      {.width = W, .height = H, .format = fmt,
       .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
       .label = "dump_color"});
  auto depth_tex = device->CreateTexture(
      {.width = W, .height = H, .format = Format::Depth32Float,
       .usage = TextureUsage::DepthStencil, .label = "dump_depth"});
  auto readback = device->CreateBuffer(
      {.size = uint64_t(W) * H * 8, // enough for RGBA16Float too
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "dump_readback"});
  REQUIRE(color_tex); REQUIRE(depth_tex); REQUIRE(readback);

  sq::SceneDocument doc;
  sq::Node node;
  node.id = 1;
  node.shape = sq::Shape::Cube;
  doc.add(node);

  sq::Camera camera;
  camera.eye = {2.5f, 2.0f, 3.5f};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  // Camera has NO default member initialisers -- an uninitialised fov makes the
  // projection garbage and every pass reject, which looks exactly like a broken
  // renderer.
  camera.fov_y_radians = 1.0472f;
  camera.aspect = 1.0f;

  device->BeginFrame();
  renderer->BeginFrame(device->CurrentFrame());
  graph::RenderGraph g(*device);
  auto color = g.ImportTexture(color_tex.get(), ResourceState::Undefined, "c");
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined, "d");
  REQUIRE(renderer->BuildFrame(g, color, depth, doc, sq::kInvalidNode, camera, W, H));
  REQUIRE(g.Compile());

  auto encoder = device->CreateCommandEncoder("dump");
  g.Execute(*encoder);
  encoder->Transition(color_tex.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color_tex.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device->Submit(*encoder);
  device->EndFrame();
  device->WaitIdle();

  const size_t bpp = (fmt == Format::RGBA8Unorm) ? 4 : 8;
  std::vector<uint8_t> raw(size_t(W) * H * bpp);
  REQUIRE(readback->Read(0, raw));

  // Normalise to RGBA8 so one set of assertions covers both formats. Half-float
  // is decoded by hand rather than pulled in as a dependency for a debug dump.
  std::vector<uint8_t> pixels(size_t(W) * H * 4);
  if (bpp == 4) {
    pixels = raw;
  } else {
    auto half_to_float = [](uint16_t h) {
      const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
      if (exp == 0) return (sign ? -1.f : 1.f) * float(man) / 1024.0f / 16384.0f;
      const uint32_t bits = (sign << 31) | ((exp + 112) << 23) | (man << 13);
      float f; std::memcpy(&f, &bits, 4); return f;
    };
    for (size_t i = 0; i < size_t(W) * H * 4; ++i) {
      uint16_t h; std::memcpy(&h, raw.data() + i * 2, 2);
      const float v = half_to_float(h);
      pixels[i] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  badlands_write_png((std::string("/tmp/shapeshifter_frame_") + tag + ".png").c_str(),
                     pixels.data(), W, H);

  auto at = [&](uint32_t x, uint32_t y) { return &pixels[(size_t(y) * W + x) * 4]; };

  // 1. The frame drew SOMETHING. The clear colour is rgb(5,5,6) at 8 bits.
  size_t lit = 0;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    if (pixels[i] > 12 || pixels[i + 1] > 12 || pixels[i + 2] > 14) ++lit;
  }
  INFO("lit texels: " << lit << " of " << (W * H));
  REQUIRE(lit > W * H / 100); // the grid alone covers far more than 1%

  // 2. The RAYMARCH ran, not just the ground plate. The cube sits at the origin
  // with the camera aimed at it, so the centre texel is its surface -- shaded by
  // normal, which is saturated in a way no grid line is.
  const uint8_t* centre = at(W / 2, H / 2);
  INFO("centre rgba(" << int(centre[0]) << "," << int(centre[1]) << ","
                      << int(centre[2]) << ")");
  const int centre_max = std::max({centre[0], centre[1], centre[2]});
  CHECK(centre_max > 100);

  // 3. The GROUND PLATE ran. Its grid lines live out at the edges where the
  // cube is not, so something above the clear colour must be there too.
  size_t edge_lit = 0;
  for (uint32_t x = 0; x < W; ++x) {
    const uint8_t* p = at(x, H - 8);
    if (p[0] > 12 || p[1] > 12 || p[2] > 14) ++edge_lit;
  }
  INFO("lit texels along the bottom edge: " << edge_lit);
  CHECK(edge_lit > 0);
}

#include <shapeshifter/ShapeshifterCore.h>

TEST_CASE("shapeshifter: attaching a layer twice does not free a live renderer",
          "[ss-rhi][metal]") {
  // attachLayer used to assign straight over impl_->device, which destroys the
  // OLD device while the renderer built from it -- pipelines, binding tables,
  // the frame allocator's buffers, a swapchain -- was still alive five lines
  // further down. SwiftUI reaches this by calling makeNSView a second time on
  // the same app-lifetime Editor.
  //
  // A WEAK GATE, said plainly: without a sanitizer a use-after-free usually
  // still "works", so a pass here is not proof. It is a live call site for the
  // ordering, which is what makes the bug reproducible under
  // -fsanitize=address rather than only reachable through the app.
  auto probe = CreateDevice({.backend = BackendKind::Metal, .label = "probe"});
  if (!probe) {
    SUCCEED("no Metal device on this host");
    return;
  }
  probe.reset();

  sq::Editor* editor = sq::Editor::create();
  REQUIRE(editor != nullptr);
  // No layer: attachLayer builds the device, compiler and renderer either way,
  // and a null layer keeps RenderFrame from asking for a drawable that a test
  // has no window to present.
  editor->attachLayer(nullptr);
  editor->setViewportSize(800.0f, 600.0f, 2.0f);
  editor->attachLayer(nullptr);
  editor->render();
  SUCCEED("the second attach rebuilt the device without freeing it underneath");
}
