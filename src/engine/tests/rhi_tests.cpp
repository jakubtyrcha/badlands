// RHI interface tests against the Null backend.
//
// No GPU is involved, so this suite runs anywhere and stays fast. It covers
// the interface contract, the shared conformance list, and the recording
// behaviour that makes Null usable as a test double.
//
// The same conformance list runs against Metal in badlands_rhi_metal_tests --
// see src/engine/tests/rhi_conformance.hpp for why that sharing matters.

#include <catch_amalgamated.hpp>

#include "engine/tests/rhi_conformance.hpp"

using namespace badlands::rhi;
namespace rhitest = badlands::rhi::test;

namespace {

std::unique_ptr<IRhiDevice> MakeNull() {
  return CreateDevice({.backend = BackendKind::Null, .label = "null_tests"});
}

}  // namespace

TEST_CASE("Null device is created through the common factory", "[rhi]") {
  auto device = MakeNull();
  REQUIRE(device);
  CHECK(device->GetBackend() == BackendKind::Null);
  CHECK_FALSE(device->IsValidationEnabled());
  CHECK(badlands::rhi::null::GetCommandLog(*device) != nullptr);
}

TEST_CASE("RHI conformance list passes on the Null backend", "[rhi]") {
  auto device = MakeNull();
  REQUIRE(device);
  rhitest::RunAllConformanceChecks(*device);
}

// Individual cases as their own TEST_CASEs too, so a failure names the feature
// rather than pointing at the aggregate.
TEST_CASE("Null: buffer round-trip", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckBufferRoundTrip(*d);
}
TEST_CASE("Null: out-of-bounds read is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckBufferBoundsAreRefused(*d);
}
TEST_CASE("Null: destroy is observable and idempotent", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDestroyIsObservableAndIdempotent(*d);
}
TEST_CASE("Null: textures and views", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTextureCreationAndViews(*d);
}
TEST_CASE("Null: compute pipeline reports its workgroup size", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckComputePipelineReportsWorkgroupSize(*d);
}
TEST_CASE("Null: reflection lookup by name", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckReflectionLookupByName(*d);
}
TEST_CASE("Null: binding table accepts every resource kind", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckBindingTableAcceptsEveryKind(*d);
}
TEST_CASE("Null: encoder and pass lifecycle", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckEncoderAndPassLifecycle(*d);
}
TEST_CASE("Null: render pass attachments are recorded", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckRenderPassAttachmentsAreRecorded(*d);
}
TEST_CASE("Null: indirect draw reads its args from the buffer", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckIndirectDrawReadsArgsFromBuffer(*d);
}
TEST_CASE("Null: buffer copy moves real bytes", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckBufferCopyMovesBytes(*d);
}
TEST_CASE("Null: transitions are recorded in order", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTransitionsAreRecorded(*d);
}
TEST_CASE("Null: compute dispatch is recorded", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckComputeDispatchIsRecorded(*d);
}

// --- Null-specific: the recording behaviour that makes it a usable double ---

TEST_CASE("Null: every LoadOp/StoreOp combination is carried through",
          "[rhi]") {
  auto device = MakeNull();
  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);

  auto color = device->CreateTexture({.width = 4, .height = 4,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget});

  const LoadOp loads[] = {LoadOp::Load, LoadOp::Clear, LoadOp::DontCare};
  const StoreOp stores[] = {StoreOp::Store, StoreOp::Discard};

  for (LoadOp load : loads) {
    for (StoreOp store : stores) {
      log->Clear();
      auto encoder = device->CreateCommandEncoder();
      RenderPassDesc desc;
      desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                        .load_op = load,
                                        .store_op = store});
      auto* pass = encoder->BeginRenderPass(desc);
      pass->End();

      const auto* begin =
          log->Find(badlands::rhi::null::RecordedCommand::Kind::BeginRenderPass);
      REQUIRE(begin != nullptr);
      CHECK(begin->first_color_load == load);
      CHECK(begin->first_color_store == store);
    }
  }
}

TEST_CASE("Null: command log preserves ordering across pass kinds", "[rhi]") {
  using Kind = badlands::rhi::null::RecordedCommand::Kind;
  auto device = MakeNull();
  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);

  auto module = device->CreateShaderModule("", rhitest::MakeTestReflection(), "m");
  auto compute = device->CreateComputePipeline({.shader = module.get()});
  auto color = device->CreateTexture({.width = 4, .height = 4,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget});

  // Compute-then-render on one encoder is the shape the GPU-driven path uses:
  // cull writes the indirect args, then the draw consumes them.
  auto encoder = device->CreateCommandEncoder("frame");
  auto* cp = encoder->BeginComputePass("cull");
  cp->SetPipeline(compute.get());
  cp->Dispatch(4);
  cp->End();

  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView()});
  auto* rp = encoder->BeginRenderPass(desc);
  rp->Draw(3);
  rp->End();
  encoder->Finish();

  const auto& all = log->All();
  REQUIRE(all.size() >= 8);
  CHECK(all[0].kind == Kind::BeginComputePass);
  CHECK(all[1].kind == Kind::SetComputePipeline);
  CHECK(all[2].kind == Kind::Dispatch);
  CHECK(all[3].kind == Kind::EndComputePass);
  CHECK(all[4].kind == Kind::BeginRenderPass);
  CHECK(all[5].kind == Kind::Draw);
  CHECK(all[6].kind == Kind::EndRenderPass);
  CHECK(all[7].kind == Kind::Finish);
}

TEST_CASE("Null: reflection merges vertex and fragment stages", "[rhi]") {
  auto device = MakeNull();

  ShaderReflection vs;
  vs.bindings.push_back({.group = 0, .slot = 0, .name = "frame",
                         .kind = BindingKind::UniformBuffer});
  ShaderReflection fs;
  // Same slot as the VS binding -- must not be duplicated.
  fs.bindings.push_back({.group = 0, .slot = 0, .name = "frame",
                         .kind = BindingKind::UniformBuffer});
  fs.bindings.push_back({.group = 1, .slot = 0, .name = "albedo",
                         .kind = BindingKind::SampledTexture});

  auto vsm = device->CreateShaderModule("", vs, "vs");
  auto fsm = device->CreateShaderModule("", fs, "fs");
  auto pipe = device->CreateRenderPipeline({.vertex_shader = vsm.get(),
                                            .fragment_shader = fsm.get(),
                                            .color_formats = {Format::RGBA8Unorm}});
  REQUIRE(pipe);

  const ShaderReflection& merged = pipe->GetReflection();
  CHECK(merged.bindings.size() == 2);
  CHECK(merged.FindBinding("frame") != nullptr);
  CHECK(merged.FindBinding("albedo") != nullptr);
}

TEST_CASE("Null: reversed-Z depth state survives the pipeline descriptor",
          "[rhi]") {
  auto device = MakeNull();
  auto vsm = device->CreateShaderModule("", ShaderReflection{}, "vs");

  RenderPipelineDesc desc;
  desc.vertex_shader = vsm.get();
  desc.color_formats = {Format::RGBA8Unorm};
  desc.depth = {.test_enabled = true,
                .write_enabled = true,
                .compare = CompareFunction::GreaterEqual,
                .format = Format::Depth32Float};
  auto pipe = device->CreateRenderPipeline(desc);
  REQUIRE(pipe);

  // Reversed-Z is a project-wide invariant; the default must not silently be
  // the conventional Less.
  CHECK(pipe->GetDesc().depth.compare == CompareFunction::GreaterEqual);
  CHECK(pipe->GetDesc().depth.format == Format::Depth32Float);
  CHECK(DepthState{}.compare == CompareFunction::GreaterEqual);
}

TEST_CASE("Null: format helpers agree with the formats we use", "[rhi]") {
  CHECK(FormatByteSize(Format::R8Unorm) == 1);
  CHECK(FormatByteSize(Format::RGBA8Unorm) == 4);
  CHECK(FormatByteSize(Format::R32Uint) == 4);
  CHECK(FormatByteSize(Format::RG32Uint) == 8);
  CHECK(FormatByteSize(Format::RGBA16Float) == 8);
  CHECK(FormatByteSize(Format::Undefined) == 0);
  CHECK(IsDepthFormat(Format::Depth32Float));
  CHECK_FALSE(IsDepthFormat(Format::R32Float));
}

TEST_CASE("Null: usage flags compose and test", "[rhi]") {
  const BufferUsage u = BufferUsage::Storage | BufferUsage::Indirect;
  CHECK(Has(u, BufferUsage::Storage));
  CHECK(Has(u, BufferUsage::Indirect));
  CHECK_FALSE(Has(u, BufferUsage::MapRead));
  CHECK(Any(u));
  CHECK_FALSE(Any(BufferUsage::None));

  // Stage visibility defaults to All, per probe B: Slang cannot tell us which
  // stages use a binding, so the RHI binds to all of them.
  CHECK(Has(ShaderStage::All, ShaderStage::Vertex));
  CHECK(Has(ShaderStage::All, ShaderStage::Compute));
  CHECK(ReflectedBinding{}.visibility == ShaderStage::All);
}
