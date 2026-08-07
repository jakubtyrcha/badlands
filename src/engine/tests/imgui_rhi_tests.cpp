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

  // UNCONDITIONAL teardown. Hand-written at the end of each case, it was
  // skipped by the Catch2 abort that a REQUIRE throws -- which happens exactly
  // when a test fails. The next MakeHarness then called ImGui::CreateContext on
  // top of a live one and every later case failed for unrelated reasons,
  // burying the first real failure.
  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;
  ~Harness() {
    if (!device) return;  // never got as far as initialising ImGui
    ImGui_ImplRHI_Shutdown();
    if (ImGui::GetCurrentContext() != nullptr) ImGui::DestroyContext();
  }
};

// Returned by value would need a move, and the destructor above makes the type
// non-movable on purpose -- an accidental copy would tear ImGui down twice.
std::unique_ptr<Harness> MakeHarness() {
  auto up = std::make_unique<Harness>();
  Harness& h = *up;
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
  return up;
}

// Runs one frame: `draw` populates the background draw list, then the graph
// clears to black and the ImGui pass composites over it.
void RenderFrame(Harness& h, const std::function<void()>& draw,
                 const float clear[4] = nullptr) {
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
      .ColorTarget(out, LoadOp::Clear, StoreOp::Store, clear ? clear : black)
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
  auto up = MakeHarness();
  Harness& h = *up;
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
}

TEST_CASE("imgui: the colour channels are not swapped", "[imgui]") {
  // Three rects, three primaries. A backend that unpacked IM_COL32 in the wrong
  // byte order renders all three and looks entirely plausible.
  auto up = MakeHarness();
  Harness& h = *up;
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
}

TEST_CASE("imgui: a clip rect actually clips", "[imgui]") {
  // SetScissor had ZERO callers outside the backends and their tests before
  // this. A rect drawn twice as wide as its clip must come back cut at the
  // clip, not at its own edge -- so this fails if the scissor is never set, set
  // to the wrong rect, or set and then overwritten.
  auto up = MakeHarness();
  Harness& h = *up;
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
}

TEST_CASE("imgui: alpha composites against what is underneath", "[imgui]") {
  // Half-alpha white over black must be grey. Without blending it is white,
  // which is the failure the blend state exists to prevent -- and it is
  // invisible to every other case here, since they all use opaque colours.
  auto up = MakeHarness();
  Harness& h = *up;
  RenderFrame(h, [] {
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(20, 20), ImVec2(80, 80), IM_COL32(255, 255, 255, 128));
  });

  const Rgba c = h.At(50, 50);
  INFO("centre = " << int(c.r) << "," << int(c.g) << "," << int(c.b) << ","
                   << int(c.a));
  CHECK(c.r > 100);
  CHECK(c.r < 160);
}

TEST_CASE("imgui: text renders through the font atlas", "[imgui]") {
  // The only case that exercises a SAMPLED TEXTURE in a raster pass, which
  // nothing outside the conformance list did before. Solid rects would pass
  // with the atlas never uploaded at all.
  auto up = MakeHarness();
  Harness& h = *up;
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
}

TEST_CASE("imgui: an empty frame adds no pass", "[imgui]") {
  // Drawing nothing must be distinguishable from failing to draw. AddPass
  // returns false in both cases, so this asserts the frame is untouched rather
  // than merely that it returned false.
  auto up = MakeHarness();
  Harness& h = *up;
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

}

TEST_CASE("imgui: consecutive frames use distinct ring slices", "[imgui]") {
  // The hazard that is INVISIBLE in one frame's pixels. With 3 frames in
  // flight, writing the indices to the same bytes every frame means frame N's
  // memcpy lands on what frames N-1 and N-2 are still reading -- torn triangles
  // and flickering glyph quads, intermittently, only once the UI is busy.
  //
  // The vertices and the params block already rode the ring; the indices were a
  // single buffer rewritten at offset 0, and nothing looked at it. rhi_lab
  // asserts exactly this for its uniforms (frame_offsets_seen); this is the
  // equivalent for a pass with three per-frame arrays instead of one.
  auto up = MakeHarness();
  Harness& h = *up;
  std::vector<uint64_t> offsets;
  for (int i = 0; i < 3; ++i) {
    // Different draw-list sizes, so a backend that merely round-robins fixed
    // slots is not enough -- the offsets must follow the real allocations.
    RenderFrame(h, [i] {
      auto* dl = ImGui::GetBackgroundDrawList();
      for (int r = 0; r <= i; ++r) {
        dl->AddRectFilled(ImVec2(10.0f + r * 12, 10), ImVec2(18.0f + r * 12, 30),
                          IM_COL32(255, 255, 255, 255));
      }
    });
    offsets.push_back(ImGui_ImplRHI_LastIndexOffset());
  }

  INFO("index offsets: " << offsets[0] << ", " << offsets[1] << ", "
                         << offsets[2]);
  // Distinct, because each frame owns a different slot of the ring.
  CHECK(offsets[0] != offsets[1]);
  CHECK(offsets[1] != offsets[2]);
  CHECK(offsets[0] != offsets[2]);
}

TEST_CASE("imgui: a frame that outgrows the ring still renders", "[imgui]") {
  // The ring hands back a DIFFERENT buffer when it grows past its block, and a
  // binding table is immutable -- so a table naming PrimaryBuffer() would draw
  // from the wrong buffer at offset 0 and render scrambled geometry, with one
  // "ring is undersized" warning as the only clue.
  //
  // Enough rects to push well past the 1 MB block: each is 4 verts x 20 bytes
  // plus 6 indices, so ~15k of them exceed it.
  auto up = MakeHarness();
  Harness& h = *up;
  RenderFrame(h, [] {
    auto* dl = ImGui::GetBackgroundDrawList();
    // One visible marker whose pixels are asserted, then the bulk offscreen so
    // it costs vertices without changing what the assertion looks at.
    dl->AddRectFilled(ImVec2(20, 20), ImVec2(60, 60), IM_COL32(255, 0, 0, 255));
    for (int i = 0; i < 16000; ++i) {
      dl->AddRectFilled(ImVec2(-100, -100), ImVec2(-90, -90),
                        IM_COL32(255, 255, 255, 255));
    }
  });

  // The ring really did grow: the vertices are no longer on the primary.
  INFO("vertex buffer == index buffer: "
       << (ImGui_ImplRHI_LastVertexBuffer() == ImGui_ImplRHI_LastIndexBuffer()));
  // ...and the marker still renders where and how it was asked to.
  const Rgba c = h.At(40, 40);
  INFO("marker = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  CHECK(c.r > 240);
  CHECK(c.g < 16);
  CHECK(c.b < 16);
}

// --- The overlay's accumulated alpha -----------------------------------------

TEST_CASE("imgui: overlapping translucent draws accumulate alpha correctly",
          "[imgui]") {
  // THE ONE PROPERTY PREMULTIPLIED BLENDING EXISTS FOR, and nothing else here
  // could see it.
  //
  // ImGui now renders into a separate OVERLAY layer that is composited over the
  // scene later, rather than straight onto an opaque surface. Every other case
  // in this file draws over an opaque black clear, where premultiplied and
  // non-premultiplied produce identical RGB and the layer's own alpha is never
  // read -- so the blend state could be wrong and every one of them would pass.
  // The same is true of the debug-line fringe assertion: over a transparent
  // clear, a SINGLE draw with blending disabled writes exactly what blending
  // would have produced. Only OVERLAP distinguishes them.
  //
  // Two 50%-alpha rects: the overlap must reach 1 - 0.5*0.5 = 0.75 (alpha 191).
  // A replacing blend leaves it at 0.5 (128), and an additive one saturates
  // to 1 (255).
  auto up = MakeHarness();
  Harness& h = *up;
  const float transparent[4] = {0, 0, 0, 0};
  RenderFrame(
      h,
      [] {
        auto* dl = ImGui::GetBackgroundDrawList();
        dl->AddRectFilled(ImVec2(10, 10), ImVec2(60, 60),
                          IM_COL32(255, 255, 255, 128));
        dl->AddRectFilled(ImVec2(40, 40), ImVec2(90, 90),
                          IM_COL32(255, 255, 255, 128));
      },
      transparent);

  const Rgba single = h.At(20, 20);   // first rect only
  const Rgba overlap = h.At(50, 50);  // both
  const Rgba empty = h.At(95, 95);    // neither
  INFO("single a=" << int(single.a) << " overlap a=" << int(overlap.a)
                   << " empty a=" << int(empty.a));

  CHECK(empty.a == 0);                            // the clear survived
  CHECK(single.a >= 126);
  CHECK(single.a <= 130);                         // one draw: 0.5
  // The load-bearing assertion. 191 is source-over; 128 would mean the second
  // draw REPLACED the first, 255 that they were added.
  CHECK(overlap.a >= 188);
  CHECK(overlap.a <= 194);

  // AND THE COLOUR, which is where the two blend states actually differ.
  //
  // PremultipliedAlphaBlend and AlphaBlend share their ALPHA factors exactly,
  // so every assertion above passes under either -- the first version of this
  // test proved that by failing to go red. They differ in the COLOUR factor:
  // the shader already multiplies by alpha, so AlphaBlend's SrcAlpha applies it
  // a SECOND time and every translucent draw comes out too dark.
  //
  // White premultiplied by its own alpha has rgb == a exactly, at any alpha.
  // Double-multiplied it is a*a: 64 instead of 128 for a half-alpha draw.
  CHECK(single.r >= single.a - 2);
  CHECK(single.r <= single.a + 2);
  CHECK(overlap.r >= overlap.a - 2);
  CHECK(overlap.r <= overlap.a + 2);
}
