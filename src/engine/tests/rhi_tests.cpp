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

// Resources the public API cannot produce, so that the paths guarding against
// them are reachable. CreateBuffer always returns a shared_ptr and every view
// has an owning texture -- which is exactly why the refusals below were
// untestable until something stood in for a backend that breaks the rule.
class UnownedBuffer final : public IBuffer {
 public:
  void Destroy() override {}
  bool IsDestroyed() const override { return false; }
  const std::string& GetLabel() const override { return label_; }
  uint64_t GetSize() const override { return 64; }
  BufferUsage GetUsage() const override { return BufferUsage::Uniform; }
  void Write(uint64_t, std::span<const uint8_t>) override {}
  bool Read(uint64_t, std::span<uint8_t>) override { return false; }

 private:
  std::string label_ = "unowned_buffer";
};

class OwnerlessView final : public ITextureView {
 public:
  void Destroy() override {}
  bool IsDestroyed() const override { return false; }
  const std::string& GetLabel() const override { return label_; }
  ITexture* GetTexture() const override { return nullptr; }
  Format GetFormat() const override { return Format::RGBA8Unorm; }
  const TextureViewDesc& GetDesc() const override { return desc_; }

 private:
  std::string label_ = "ownerless_view";
  TextureViewDesc desc_;
};

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
TEST_CASE("rhi: an entry that cannot be retained is refused loudly", "[rhi]") {
  // RetainBindingResources exists to stop a binding table outliving the
  // resources it references. Skipping an entry it cannot retain reintroduces
  // that exact use-after-free -- and the caller has already been told it may
  // drop its handle, so silence here is the worst possible answer.
  UnownedBuffer buf;
  OwnerlessView view;
  std::vector<BindingEntry> entries = {
      {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = &buf},
      {.slot = 1, .kind = BindingKind::SampledTexture, .texture_view = &view},
  };

  std::vector<std::shared_ptr<IResource>> retained;
  const std::string log = rhitest::CaptureLog(
      [&] { retained = RetainBindingResources(entries, "fake_table"); });

  INFO(log);
  CHECK(retained.empty());
  CHECK(log.find("not shared_ptr-owned") != std::string::npos);
  CHECK(log.find("no owning texture") != std::string::npos);
  // Names the table and the slot, or the message cannot be acted on.
  CHECK(log.find("fake_table") != std::string::npos);
  CHECK(log.find("slot 0") != std::string::npos);
  CHECK(log.find("slot 1") != std::string::npos);
}

TEST_CASE("rhi: a retainable entry is retained silently", "[rhi]") {
  // The paired green. Without it the case above would pass just as well
  // against a version that refuses everything.
  auto device = MakeNull();
  auto ubo = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "real_ubo"});
  auto tex = device->CreateTexture({.width = 4, .height = 4,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled,
                                    .label = "real_tex"});
  std::vector<BindingEntry> entries = {
      {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
      {.slot = 1, .kind = BindingKind::SampledTexture,
       .texture_view = tex->GetDefaultView()},
  };

  std::vector<std::shared_ptr<IResource>> retained;
  const std::string log = rhitest::CaptureLog(
      [&] { retained = RetainBindingResources(entries, "real_table"); });

  INFO(log);
  CHECK(retained.size() == 2);
  CHECK(log.empty());
  // The view's OWNER is what gets retained -- keeping the texture alive keeps
  // the view alive, which is the point.
  CHECK(retained[1].get() == static_cast<IResource*>(tex.get()));
}

TEST_CASE("Null: sliced views honour their range", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSlicedViewsHonourTheirRange(*d);
}
TEST_CASE("Null: out-of-range views are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckOutOfRangeViewsAreRefused(*d);
}
TEST_CASE("Null: submissions retire", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSubmissionsRetire(*d);
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
TEST_CASE("Null: indexed draw honours first_index", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckIndexedDrawHonoursFirstIndex(*d);
}
TEST_CASE("Null: texture upload is sampled", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTextureUploadIsSampled(*d);
}
TEST_CASE("Null: LoadOp preserves previous contents", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckLoadOpPreservesPreviousContents(*d);
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

  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto compute = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
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

TEST_CASE("Null: views survive Destroy on their texture", "[rhi]") {
  auto d = MakeNull();
  REQUIRE(d);
  rhitest::CheckViewsSurviveTextureDestroy(*d);
}
TEST_CASE("Null: a binding table retains its resources", "[rhi]") {
  auto d = MakeNull();
  REQUIRE(d);
  rhitest::CheckBindingTableRetainsItsResources(*d);
}
