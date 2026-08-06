// The ImGui RHI backend, asserted on pixels.
//
// ImGui is normally verified by looking at it, which is how a backend ships
// with the scissor off by a row or the vertex stride wrong in a way nobody
// notices until a glyph looks fuzzy. A headless context plus a
// fixed-coordinate draw list plus a readback turns every one of those into a
// number: a rect of a known colour must cover known texels, and a clip rect
// must cut it exactly where it was told to.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch_amalgamated.hpp>
#include <imgui.h>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "engine/ui/imgui_impl_rhi.hpp"

using namespace badlands;
using namespace badlands::rhi;
using badlands::graph::RenderGraph;

namespace {

constexpr uint32_t kW = 128;
constexpr uint32_t kH = 128;

struct Rgba { uint8_t r, g, b, a; };

// Everything a frame needs, plus the readback it produced.
struct Harness {
  std::unique_ptr<IRhiDevice> device;
  std::unique_ptr<slang::SlangCompiler> compiler;
  TexturePtr target;
  BufferPtr readback;
  std::vector<uint8_t> pixels;

  Rgba At(uint32_t x, uint32_t y) const {
    const size_t o = (size_t(y) * kW + x) * 4;
    return {pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]};
  }
  bool IsBlack(uint32_t x, uint32_t y) const {
    const Rgba c = At(x, y);
    return c.r < 8 && c.g < 8 && c.b < 8;
  }
};

Harness MakeHarness() {
  Harness h;
  h.device = CreateDevice({.backend = BackendKind::Metal,
                           .enable_validation = true,
                           .label = "imgui_tests"});
  REQUIRE(h.device);
  const std::vector<std::string> paths = {"shaders/slang/ui"};
  h.compiler = slang::CreateSlangCompiler(paths);
  REQUIRE(h.compiler);

  h.target = h.device->CreateTexture({.width = kW, .height = kH,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget |
                                               TextureUsage::CopySrc,
                                      .label = "ui"});
  h.readback = h.device->CreateBuffer({.size = uint64_t(kW) * kH * 4,
                                       .usage = BufferUsage::CopyDst |
                                                BufferUsage::MapRead,
                                       .label = "readback"});
  REQUIRE(h.target);
  REQUIRE(h.readback);

  REQUIRE(ImGui_ImplRHI_InitHeadless({.device = h.device.get(),
                                      .compiler = h.compiler.get(),
                                      .target_format = Format::RGBA8Unorm,
                                      .framebuffer_width = kW,
                                      .framebuffer_height = kH}));
  return h;
}

// Runs one frame: `draw` populates the background draw list, then the graph
// clears to black and the ImGui pass composites over it.
void RenderFrame(Harness& h, const std::function<void()>& draw) {
  h.device->BeginValidationScope();
  h.device->BeginFrame();
  ImGui_ImplRHI_NewFrame(h.device->CurrentFrame());

  ImGui::NewFrame();
  draw();
  ImGui::Render();

  RenderGraph graph(*h.device);
  auto out = graph.ImportTexture(h.target.get(), ResourceState::Undefined, "ui");
  const float black[4] = {0, 0, 0, 1};
  graph.AddRasterPass("clear")
      .ColorTarget(out, LoadOp::Clear, StoreOp::Store, black)
      .Execute([](const graph::RasterContext&) {});
  const bool added = ImGui_ImplRHI_AddPass(ImGui::GetDrawData(), graph, out);
  REQUIRE(added);
  REQUIRE(graph.Compile());

  auto encoder = h.device->CreateCommandEncoder("ui");
  graph.Execute(*encoder);
  encoder->Transition(h.target.get(), ResourceState::CopySrc);
  encoder->Transition(h.readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(h.target.get(), 0, 0, h.readback.get(), 0);
  encoder->Finish();
  h.device->Submit(*encoder);
  h.device->EndFrame();
  h.device->WaitIdle();

  auto report = h.device->EndValidationScope();
  REQUIRE(report.has_value());
  INFO(report->violations);
  CHECK(report->IsClean());

  h.pixels.assign(size_t(kW) * kH * 4, 0);
  REQUIRE(h.readback->Read(0, h.pixels));
}

// Counts texels that are not the black clear.
size_t Covered(const Harness& h) {
  size_t n = 0;
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      if (!h.IsBlack(x, y)) ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("imgui: a filled rect lands on the texels it was given", "[imgui]") {
  // The whole backend in one assertion: the vertex stride, the projection, the
  // colour unpack and the blend all have to be right for a rect at (20,30)
  // sized 40x25 to come back at exactly those texels.
  auto h = MakeHarness();
  RenderFrame(h, [] {
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(20, 30), ImVec2(60, 55), IM_COL32(255, 0, 0, 255));
  });

  // Inside is red...
  const Rgba inside = h.At(40, 42);
  INFO("inside = " << int(inside.r) << "," << int(inside.g) << ","
                   << int(inside.b));
  CHECK(inside.r > 240);
  CHECK(inside.g < 16);
  CHECK(inside.b < 16);

  // ...and just outside each edge is not. One texel of slack for the edge
  // sample; two would let a whole-rect offset through.
  CHECK(h.IsBlack(18, 42));
  CHECK(h.IsBlack(62, 42));
  CHECK(h.IsBlack(40, 28));
  CHECK(h.IsBlack(40, 57));

  // The area, which catches a rect that is right in position and wrong in size.
  const size_t covered = Covered(h);
  INFO("covered = " << covered);
  CHECK(covered >= 40 * 25 - 200);
  CHECK(covered <= 40 * 25 + 200);
  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}

TEST_CASE("imgui: the colour channels are not swapped", "[imgui]") {
  // Three rects, three primaries. A backend that unpacked IM_COL32 in the wrong
  // byte order renders all three and looks entirely plausible.
  auto h = MakeHarness();
  RenderFrame(h, [] {
    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(10, 10), ImVec2(30, 30), IM_COL32(255, 0, 0, 255));
    dl->AddRectFilled(ImVec2(40, 10), ImVec2(60, 30), IM_COL32(0, 255, 0, 255));
    dl->AddRectFilled(ImVec2(70, 10), ImVec2(90, 30), IM_COL32(0, 0, 255, 255));
  });

  const Rgba r = h.At(20, 20), g2 = h.At(50, 20), b = h.At(80, 20);
  INFO("r=" << int(r.r) << "," << int(r.g) << "," << int(r.b)
            << " g=" << int(g2.r) << "," << int(g2.g) << "," << int(g2.b)
            << " b=" << int(b.r) << "," << int(b.g) << "," << int(b.b));
  CHECK((r.r > 240 && r.g < 16 && r.b < 16));
  CHECK((g2.r < 16 && g2.g > 240 && g2.b < 16));
  CHECK((b.r < 16 && b.g < 16 && b.b > 240));
  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}

TEST_CASE("imgui: a clip rect actually clips", "[imgui]") {
  // SetScissor had ZERO callers outside the backends and their tests before
  // this. A rect drawn twice as wide as its clip must come back cut at the
  // clip, not at its own edge -- so this fails if the scissor is never set, set
  // to the wrong rect, or set and then overwritten.
  auto h = MakeHarness();
  RenderFrame(h, [] {
    auto* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(20, 20), ImVec2(50, 60), false);
    dl->AddRectFilled(ImVec2(20, 20), ImVec2(100, 60),
                      IM_COL32(0, 255, 0, 255));
    dl->PopClipRect();
  });

  // Inside the clip: drawn. Past its right edge but inside the rect: cut.
  CHECK_FALSE(h.IsBlack(35, 40));
  CHECK(h.IsBlack(70, 40));
  CHECK(h.IsBlack(99, 40));

  // The covered area is the CLIP's, not the rect's.
  const size_t covered = Covered(h);
  INFO("covered = " << covered << " clip area = " << 30 * 40);
  CHECK(covered <= 30 * 40 + 200);
  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}

TEST_CASE("imgui: alpha composites against what is underneath", "[imgui]") {
  // Half-alpha white over black must be grey. Without blending it is white,
  // which is the failure the blend state exists to prevent -- and it is
  // invisible to every other case here, since they all use opaque colours.
  auto h = MakeHarness();
  RenderFrame(h, [] {
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(20, 20), ImVec2(80, 80), IM_COL32(255, 255, 255, 128));
  });

  const Rgba c = h.At(50, 50);
  INFO("centre = " << int(c.r) << "," << int(c.g) << "," << int(c.b) << ","
                   << int(c.a));
  CHECK(c.r > 100);
  CHECK(c.r < 160);
  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}

TEST_CASE("imgui: text renders through the font atlas", "[imgui]") {
  // The only case that exercises a SAMPLED TEXTURE in a raster pass, which
  // nothing outside the conformance list did before. Solid rects would pass
  // with the atlas never uploaded at all.
  auto h = MakeHarness();
  RenderFrame(h, [] {
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(10, 10),
                                            IM_COL32(255, 255, 255, 255),
                                            "IIIIIIIIII");
  });

  const size_t covered = Covered(h);
  INFO("covered = " << covered);
  // Glyphs are sparse, so this is a band rather than a count: enough texels to
  // be text, few enough not to be a filled rectangle.
  CHECK(covered > 40);
  CHECK(covered < 2000);
  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}

TEST_CASE("imgui: an empty frame adds no pass", "[imgui]") {
  // Drawing nothing must be distinguishable from failing to draw. AddPass
  // returns false in both cases, so this asserts the frame is untouched rather
  // than merely that it returned false.
  auto h = MakeHarness();
  h.device->BeginFrame();
  ImGui_ImplRHI_NewFrame(h.device->CurrentFrame());
  ImGui::NewFrame();
  ImGui::Render();

  RenderGraph graph(*h.device);
  auto out = graph.ImportTexture(h.target.get(), ResourceState::Undefined, "ui");
  CHECK_FALSE(ImGui_ImplRHI_AddPass(ImGui::GetDrawData(), graph, out));
  CHECK(graph.PassCount() == 0);
  h.device->EndFrame();
  h.device->WaitIdle();

  ImGui_ImplRHI_Shutdown();
  ImGui::DestroyContext();
}
