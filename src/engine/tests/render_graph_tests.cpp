// Render graph tests, on the Null backend, with no GPU.
//
// THIS IS THE POINT OF BUILDING THE GRAPH NOW. Its hardest property is "did it
// declare the right resource transitions, in the right order?", and on Metal
// that is unfalsifiable: Metal tracks hazards itself and renders correctly
// whether or not a single transition was declared. The validation decorator
// checks the declared intent as bookkeeping over the command stream, so the
// property is assertable here, in the fast suite, on any machine -- rather than
// surfacing as corrupted output on a DX12 box that does not exist yet.

#include <catch_amalgamated.hpp>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/null/null_rhi.hpp"

using namespace badlands::rhi;
using namespace badlands::graph;
namespace null = badlands::rhi::null;

namespace {

std::unique_ptr<IRhiDevice> MakeDevice(bool validation = true) {
  return CreateDevice({.backend = BackendKind::Null,
                       .enable_validation = validation,
                       .label = "graph_tests"});
}

TexturePtr MakeTarget(IRhiDevice& d, const char* label = "target") {
  return d.CreateTexture({.width = 16, .height = 16,
                          .format = Format::RGBA8Unorm,
                          .usage = TextureUsage::RenderTarget |
                                   TextureUsage::Sampled,
                          .label = label});
}

TexturePtr MakeDepth(IRhiDevice& d, const char* label = "depth") {
  return d.CreateTexture({.width = 16, .height = 16,
                          .format = Format::Depth32Float,
                          .usage = TextureUsage::DepthStencil,
                          .label = label});
}

// Runs a graph inside a validation scope and returns what the decorator saw.
// Empty means clean. REQUIREs that a report exists at all -- nullopt would mean
// nothing was checked, which a caller could otherwise read as success.
std::string RunUnderValidation(IRhiDevice& d, RenderGraph& g) {
  d.BeginValidationScope();
  d.BeginFrame();
  auto encoder = d.CreateCommandEncoder("graph");
  g.Execute(*encoder);
  encoder->Finish();
  d.Submit(*encoder);
  d.EndFrame();
  d.WaitIdle();
  auto report = d.EndValidationScope();
  REQUIRE(report.has_value());
  return report->violations;
}

}  // namespace

// --- Compile-time refusals --------------------------------------------------

TEST_CASE("graph: a pass reading what nothing writes is refused", "[graph]") {
  // The classic graph bug: the pass renders whatever the memory happened to
  // hold, which is usually the previous frame and occasionally garbage. It has
  // to be a compile error, because at record time there is nothing to see.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  auto never_written =
      g.CreateTexture({.width = 8, .height = 8, .format = Format::RGBA8Unorm,
                       .usage = TextureUsage::Sampled, .label = "orphan"});

  g.AddRasterPass("reads_orphan")
      .ColorTarget(out)
      .Reads(never_written)
      .Execute([](const RasterContext&) {});

  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a pass reading what a LATER pass writes is refused",
          "[graph]") {
  // Ordering is the whole point. Declaring the producer second means it runs
  // second, so the consumer samples a texture nothing has written yet -- and
  // the guard above cannot see that if producedness is decided at declaration
  // time rather than walked in order.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto mid = g.CreateTexture({.width = 16, .height = 16,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::RenderTarget |
                                       TextureUsage::Sampled,
                              .label = "mid"});

  // Consumer FIRST.
  g.AddRasterPass("consume").ColorTarget(out).Reads(mid).Execute(
      [](const RasterContext&) {});
  g.AddRasterPass("produce").ColorTarget(mid).Execute(
      [](const RasterContext&) {});

  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a pass may read what it also writes, once produced",
          "[graph]") {
  // The ping-pong case, so the order-aware check does not overcorrect into
  // refusing a legitimate read-modify-write.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto mid = g.CreateTexture({.width = 16, .height = 16,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::RenderTarget |
                                       TextureUsage::Sampled,
                              .label = "mid"});

  g.AddRasterPass("produce").ColorTarget(mid).Execute(
      [](const RasterContext&) {});
  // Reads mid and writes it again, in that order.
  g.AddRasterPass("refine").ColorTarget(mid, LoadOp::Load).Reads(mid).Execute(
      [](const RasterContext&) {});
  g.AddRasterPass("present").ColorTarget(out).Reads(mid).Execute(
      [](const RasterContext&) {});

  CHECK(g.Compile());
}

TEST_CASE("graph: an unknown resource handle is refused", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");

  g.AddRasterPass("reads_nothing")
      .ColorTarget(out)
      .Reads(ResourceHandle{})  // default-constructed: invalid
      .Execute([](const RasterContext&) {});

  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a pass with no callback is refused", "[graph]") {
  // A pass that declares resources and records nothing still forces its
  // transitions, so it is not harmless -- it is a barrier with no work.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  g.AddRasterPass("empty").ColorTarget(out);

  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a raster pass with neither colour nor depth is refused",
          "[graph]") {
  // Colour alone is no longer the test: a depth-only shadow pass renders
  // somewhere, it just renders depth. Declaring NEITHER is still nowhere.
  auto d = MakeDevice();
  RenderGraph g(*d);
  g.AddRasterPass("nowhere").Execute([](const RasterContext&) {});
  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: depth-testing what nothing has written is refused",
          "[graph]") {
  // Same defect as reading an unwritten texture, wearing a different
  // attachment: the pass tests against whatever the memory held. It has to be
  // refused for the same reason, which is why DepthReadOnly counts as a read.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  auto depth = g.CreateTexture({.width = 16, .height = 16,
                                .format = Format::Depth32Float,
                                .usage = TextureUsage::DepthStencil,
                                .label = "depth"});
  g.AddRasterPass("overlay")
      .ColorTarget(out)
      .DepthReadOnly(depth)
      .Execute([](const RasterContext&) {});
  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a colour texture declared as depth is refused", "[graph]") {
  // The backend would refuse the render pass anyway; refusing here is what
  // names the pass and the resource instead of failing at record time.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  auto not_depth = MakeTarget(*d, "not_depth");
  auto wrong = g.ImportTexture(not_depth.get(), ResourceState::Undefined,
                               "wrong");
  g.AddRasterPass("mistake")
      .ColorTarget(out)
      .DepthTarget(wrong)
      .Execute([](const RasterContext&) {});
  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: Execute without a successful Compile records nothing",
          "[graph]") {
  auto d = MakeDevice(/*validation=*/false);
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  g.AddRasterPass("unchecked")
      .ColorTarget(g.ImportTexture(target.get(), ResourceState::Undefined))
      .Execute([](const RasterContext&) {});
  // Deliberately no Compile().

  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);
  log->Clear();
  auto encoder = d->CreateCommandEncoder("graph");
  g.Execute(*encoder);
  CHECK(log->Count(null::RecordedCommand::Kind::BeginRenderPass) == 0);
}

// --- The transitions, which is what only Null can see -----------------------

TEST_CASE("graph: a single pass declares its colour target", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");

  bool ran = false;
  const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  g.AddRasterPass("clear")
      .ColorTarget(out, LoadOp::Clear, StoreOp::Store, clear)
      .Execute([&](const RasterContext& ctx) {
        ran = true;
        CHECK(ctx.width == 16);
        CHECK(ctx.height == 16);
      });
  REQUIRE(g.Compile());

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(ran);
}

TEST_CASE("graph: a produce-then-consume chain declares both states",
          "[graph]") {
  // Pass 1 renders into a transient texture, pass 2 samples it. The graph must
  // move it RenderTarget -> ShaderRead between them; forgetting that is exactly
  // the barrier DX12 needs and Metal does not.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto mid = g.CreateTexture({.width = 16, .height = 16,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::RenderTarget |
                                       TextureUsage::Sampled,
                              .label = "mid"});

  g.AddRasterPass("produce").ColorTarget(mid).Execute(
      [](const RasterContext&) {});
  g.AddRasterPass("consume").ColorTarget(out).Reads(mid).Execute(
      [](const RasterContext&) {});
  REQUIRE(g.Compile());

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());

  // The transitions really were recorded, not merely absent from complaints.
  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);
  CHECK(log->Count(null::RecordedCommand::Kind::Transition) >= 3);
  CHECK(log->Count(null::RecordedCommand::Kind::BeginRenderPass) == 2);
}

TEST_CASE("graph: passes execute in declaration order", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");

  std::vector<int> seen;
  for (int i = 0; i < 3; ++i) {
    g.AddRasterPass("p" + std::to_string(i))
        .ColorTarget(out, LoadOp::Load)
        .Execute([&seen, i](const RasterContext&) { seen.push_back(i); });
  }
  REQUIRE(g.Compile());
  CHECK(g.Order() == std::vector<uint32_t>{0, 1, 2});

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(seen == std::vector<int>{0, 1, 2});
}

TEST_CASE("graph: a compute pass declares its writes", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto buffer = d->CreateBuffer({.size = 256,
                                 .usage = BufferUsage::Storage,
                                 .label = "counts"});
  REQUIRE(buffer);
  auto counts = g.ImportBuffer(buffer.get(), ResourceState::Undefined, "counts");

  bool ran = false;
  g.AddComputePass("count").Writes(counts).Execute(
      [&](const ComputeContext& ctx) {
        ran = true;
        CHECK(ctx.pass != nullptr);
      });
  REQUIRE(g.Compile());

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(ran);
}

TEST_CASE("graph: an imported entry state is believed, not assumed", "[graph]") {
  // A texture already in ShaderRead, read again: nothing to declare. If the
  // graph assumed Undefined it would emit a redundant transition, which is
  // harmless on Metal and a wasted barrier on DX12 -- and, more to the point,
  // means the entry state is being ignored.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto src = MakeTarget(*d, "already_readable");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto pre = g.ImportTexture(src.get(), ResourceState::ShaderRead, "pre");

  g.AddRasterPass("sample").ColorTarget(out).Reads(pre).Execute(
      [](const RasterContext&) {});
  REQUIRE(g.Compile());

  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);
  log->Clear();
  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  // Only the backbuffer needed moving.
  CHECK(log->Count(null::RecordedCommand::Kind::Transition) == 1);
}

TEST_CASE("graph: transient textures are created at Compile", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  auto mid = g.CreateTexture({.width = 32, .height = 24,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::RenderTarget |
                                       TextureUsage::Sampled,
                              .label = "mid"});

  uint32_t seen_w = 0, seen_h = 0;
  g.AddRasterPass("produce").ColorTarget(mid).Execute(
      [&](const RasterContext& ctx) {
        seen_w = ctx.width;
        seen_h = ctx.height;
      });
  g.AddRasterPass("consume").ColorTarget(out).Reads(mid).Execute(
      [](const RasterContext&) {});
  REQUIRE(g.Compile());
  RunUnderValidation(*d, g);

  // The context reports the ATTACHMENT's size, not the backbuffer's -- a pass
  // rendering into a half-res target must get the half-res viewport.
  CHECK(seen_w == 32);
  CHECK(seen_h == 24);
}

TEST_CASE("graph: a compiled graph can be executed more than once", "[graph]") {
  // Compile() is the expensive step and Execute() advertises no once-per-compile
  // restriction, so caching a compiled graph across frames is the obvious
  // optimisation. It has to actually work: the first Execute left every resource
  // in its FINAL state, so the second emitted no transitions at all -- correct on
  // Metal, which tracks hazards itself, and a use-before-barrier on DX12.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto mid = g.CreateTexture({.width = 16, .height = 16,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::RenderTarget |
                                       TextureUsage::Sampled,
                              .label = "mid"});

  g.AddRasterPass("produce").ColorTarget(mid).Execute(
      [](const RasterContext&) {});
  g.AddRasterPass("consume").ColorTarget(out).Reads(mid).Execute(
      [](const RasterContext&) {});
  REQUIRE(g.Compile());

  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);

  size_t first_transitions = 0;
  for (int run = 0; run < 3; ++run) {
    log->Clear();
    const std::string violations = RunUnderValidation(*d, g);
    INFO("run " << run << ": " << violations);
    CHECK(violations.empty());
    const size_t n = log->Count(null::RecordedCommand::Kind::Transition);
    INFO("run " << run << " emitted " << n << " transitions");
    if (run == 0) {
      first_transitions = n;
      CHECK(n > 0);
    } else {
      // The SAME barriers every time. Fewer means a later frame is running
      // unbarriered; more means state is accumulating.
      CHECK(n == first_transitions);
    }
  }
}

// --- Depth, which is what the editor's frame is shaped like -----------------

TEST_CASE("graph: depth written then depth-tested declares DepthWrite then "
          "DepthRead", "[graph]") {
  // Shapeshifter's frame in miniature, and the reason depth reached the graph
  // at all: geometry writes the buffer, then an overlay tests against it
  // without writing. The two need DIFFERENT states, and a graph that emitted
  // DepthWrite for both would render correctly on Metal and corrupt on DX12.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto depth_tex = MakeDepth(*d);
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined,
                               "depth");

  int ran = 0;
  g.AddRasterPass("geometry")
      .ColorTarget(out)
      .DepthTarget(depth, LoadOp::Clear, StoreOp::Store, 0.0f)
      .Execute([&](const RasterContext& ctx) {
        ++ran;
        CHECK(ctx.width == 16);
        CHECK(ctx.height == 16);
      });
  g.AddRasterPass("overlay")
      .ColorTarget(out, LoadOp::Load)
      .DepthReadOnly(depth)
      .Execute([&](const RasterContext&) { ++ran; });
  REQUIRE(g.Compile());

  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);
  log->Clear();
  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(ran == 2);
  // Both passes began, and the depth resource really did move between states
  // rather than being declared once and left there.
  CHECK(log->Count(null::RecordedCommand::Kind::BeginRenderPass) == 2);
  CHECK(log->Count(null::RecordedCommand::Kind::Transition) >= 3);
}

TEST_CASE("graph: a depth-only pass renders, and sizes itself from depth",
          "[graph]") {
  // A shadow pass has no colour attachment to take an extent from. Falling back
  // to the depth attachment is what keeps the viewport matching what the pass
  // renders into -- the same guarantee the colour path already gives.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto depth_tex = MakeDepth(*d, "shadow");
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined,
                               "shadow");

  bool ran = false;
  g.AddRasterPass("shadow")
      .DepthTarget(depth)
      .Execute([&](const RasterContext& ctx) {
        ran = true;
        CHECK(ctx.width == 16);
        CHECK(ctx.height == 16);
      });
  REQUIRE(g.Compile());

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(ran);
}

TEST_CASE("graph: a transient depth texture is created at Compile", "[graph]") {
  // The editor owns no depth texture of its own once ported -- the graph makes
  // it. Sizing and lifetime therefore have to work for a depth format, not just
  // for colour.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto depth = g.CreateTexture({.width = 16, .height = 16,
                                .format = Format::Depth32Float,
                                .usage = TextureUsage::DepthStencil,
                                .label = "transient_depth"});

  bool ran = false;
  g.AddRasterPass("geometry")
      .ColorTarget(out)
      .DepthTarget(depth)
      .Execute([&](const RasterContext&) { ran = true; });
  REQUIRE(g.Compile());

  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());
  CHECK(ran);
}

// --- The three the first draft of depth got wrong ---------------------------

TEST_CASE("graph: a TRANSIENT colour texture declared as depth is refused",
          "[graph]") {
  // The imported case was covered and the transient one was not, which is
  // backwards: transients are created AFTER validation runs, so asking the
  // texture for its format read a null pointer and the check quietly did not
  // run -- for the graph-owned depth buffer that is the common case.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto target = MakeTarget(*d);
  auto out = g.ImportTexture(target.get(), ResourceState::Undefined, "out");
  auto wrong = g.CreateTexture({.width = 16, .height = 16,
                                .format = Format::RGBA8Unorm,  // not depth
                                .usage = TextureUsage::RenderTarget,
                                .label = "not_depth_transient"});
  g.AddRasterPass("mistake")
      .ColorTarget(out)
      .DepthTarget(wrong)
      .Execute([](const RasterContext&) {});
  CHECK_FALSE(g.Compile());
}

TEST_CASE("graph: a read-only depth pass STORES depth, so the next pass can "
          "still read it", "[graph]") {
  // Discard does not mean "I changed nothing" -- it means the contents are
  // undefined afterwards. Two overlays sharing one depth buffer is the exact
  // shape DepthReadOnly exists for, and discarding in the first makes the
  // second test against garbage. Metal renders that plausibly, which is why the
  // op is recorded and asserted rather than eyeballed.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto backbuffer = MakeTarget(*d, "backbuffer");
  auto depth_tex = MakeDepth(*d);
  auto out = g.ImportTexture(backbuffer.get(), ResourceState::Undefined, "out");
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined,
                               "depth");

  g.AddRasterPass("geometry")
      .ColorTarget(out)
      .DepthTarget(depth)
      .Execute([](const RasterContext&) {});
  g.AddRasterPass("overlay_a")
      .ColorTarget(out, LoadOp::Load)
      .DepthReadOnly(depth)
      .Execute([](const RasterContext&) {});
  g.AddRasterPass("overlay_b")
      .ColorTarget(out, LoadOp::Load)
      .DepthReadOnly(depth)
      .Execute([](const RasterContext&) {});
  REQUIRE(g.Compile());

  auto* log = null::GetCommandLog(*d);
  REQUIRE(log != nullptr);
  log->Clear();
  const std::string violations = RunUnderValidation(*d, g);
  INFO(violations);
  CHECK(violations.empty());

  REQUIRE(log->Count(null::RecordedCommand::Kind::BeginRenderPass) == 3);
  for (size_t i = 0; i < 3; ++i) {
    const auto* rp = log->Find(null::RecordedCommand::Kind::BeginRenderPass, i);
    REQUIRE(rp != nullptr);
    CAPTURE(i, rp->label);
    CHECK(rp->has_depth);
    // NOT Discard, in any of the three: the geometry pass has depth to keep,
    // and each overlay has to leave it intact for the one after it.
    CHECK(rp->depth_store == StoreOp::Store);
  }
  // The overlays load what geometry wrote, and declare themselves read-only.
  for (size_t i = 1; i < 3; ++i) {
    const auto* rp = log->Find(null::RecordedCommand::Kind::BeginRenderPass, i);
    CAPTURE(i);
    CHECK(rp->depth_load == LoadOp::Load);
    CHECK(rp->depth_read_only);
  }
}

TEST_CASE("graph: a pass whose only depth is read-only renders nowhere",
          "[graph]") {
  // No colour to draw into and no depth to update. "Has a depth target" was too
  // weak a test for renders-somewhere; it has to be depth the pass WRITES.
  auto d = MakeDevice();
  RenderGraph g(*d);
  auto depth_tex = MakeDepth(*d);
  auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined,
                               "depth");
  g.AddRasterPass("nowhere")
      .DepthReadOnly(depth)
      .Execute([](const RasterContext&) {});
  CHECK_FALSE(g.Compile());
}
