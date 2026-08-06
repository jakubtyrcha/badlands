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

TEST_CASE("graph: a raster pass with no colour target is refused", "[graph]") {
  auto d = MakeDevice();
  RenderGraph g(*d);
  g.AddRasterPass("nowhere").Execute([](const RasterContext&) {});
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
