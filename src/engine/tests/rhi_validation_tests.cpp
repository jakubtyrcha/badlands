// Validation decorator tests, against the Null backend.
//
// Every check gets a test that provokes it, because a validation layer nobody
// has seen fire is indistinguishable from one that does nothing. The
// resource-state intent cases are the important ones: they are what the DX12
// backend will lean on, and they run here with no GPU at all.

#include <catch_amalgamated.hpp>

#include "engine/tests/rhi_conformance.hpp"

using namespace badlands::rhi;
namespace rhitest = badlands::rhi::test;

namespace {

std::unique_ptr<IRhiDevice> MakeValidated() {
  return CreateDevice({.backend = BackendKind::Null,
                       .enable_validation = true,
                       .label = "validated"});
}

// Runs `fn` inside a validation scope and returns whatever was observed.
template <typename Fn>
std::optional<std::string> Observe(IRhiDevice& device, Fn&& fn) {
  device.BeginValidationScope();
  fn();
  return device.EndValidationScope();
}

// A texture usable as a color attachment.
TexturePtr MakeColorTarget(IRhiDevice& d, const char* label = "color") {
  return d.CreateTexture({.width = 8, .height = 8,
                          .format = Format::RGBA8Unorm,
                          .usage = TextureUsage::RenderTarget,
                          .label = label});
}

}  // namespace

TEST_CASE("validation: enabled device reports itself as such", "[rhi][validation]") {
  auto device = MakeValidated();
  REQUIRE(device);
  CHECK(device->IsValidationEnabled());
  // Still the Null backend underneath -- the decorator is transparent.
  CHECK(device->GetBackend() == BackendKind::Null);
}

TEST_CASE("validation: a clean scope observes nothing", "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    auto buf = device->CreateBuffer(
        {.size = 64, .usage = BufferUsage::CopyDst, .label = "ok"});
    (void)buf;
  });
  CHECK_FALSE(observed.has_value());
}

TEST_CASE("validation: the command log is reachable through the decorator",
          "[rhi][validation]") {
  // Without this, every GetCommandLog()-guarded assertion in the conformance
  // list silently skips when the device is validated -- so the "runs clean
  // under validation" case below was only ever checking half of what it looked
  // like it checked. A log assertion that quietly does not run is worse than
  // one that fails.
  auto device = MakeValidated();
  REQUIRE(device);
  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);

  auto encoder = device->CreateCommandEncoder("through_decorator");
  encoder->Finish();
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::Finish) == 1);
}

TEST_CASE("validation: the full conformance list runs clean under validation",
          "[rhi][validation]") {
  // The strongest single assertion here: the shared behavioural list is not
  // just passing, it is passing without tripping a single check. That makes
  // the conformance suite an example of correct usage rather than merely a
  // set of calls that happen to work.
  auto device = MakeValidated();
  auto observed =
      Observe(*device, [&] { rhitest::RunAllConformanceChecks(*device); });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
}

// --- Ordinary misuse --------------------------------------------------------

TEST_CASE("validation: a declared-but-unbound slot is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto ubo = device->CreateBuffer({.size = 64, .usage = BufferUsage::Uniform});

  auto observed = Observe(*device, [&] {
    BindingTableDesc d;
    d.compute_pipeline = pipe.get();
    d.group = 0;
    d.label = "partial";
    // The reflection declares slots 0..3; bind only slot 0.
    d.entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                  .buffer = ubo.get()}};
    auto table = device->CreateBindingTable(d);
    (void)table;
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("not bound") != std::string::npos);
}

TEST_CASE("validation: a slot bound with the wrong kind is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto ubo = device->CreateBuffer({.size = 64, .usage = BufferUsage::Uniform});
  auto ssbo = device->CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
  auto tex = device->CreateTexture({.width = 4, .height = 4,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled});
  auto samp = device->CreateSampler({});

  auto observed = Observe(*device, [&] {
    BindingTableDesc d;
    d.compute_pipeline = pipe.get();
    d.label = "wrongkind";
    d.entries = {
        // Slot 0 is a UniformBuffer in reflection; bind a storage buffer.
        {.slot = 0, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
        {.slot = 1, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
        {.slot = 2, .kind = BindingKind::SampledTexture,
         .texture_view = tex->GetDefaultView()},
        {.slot = 3, .kind = BindingKind::Sampler, .sampler = samp.get()},
    };
    (void)device->CreateBindingTable(d);
    (void)ubo;
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("kind") != std::string::npos);
}

TEST_CASE("validation: using a destroyed resource is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto buf = device->CreateBuffer(
      {.size = 32, .usage = BufferUsage::Storage, .label = "dead"});
  buf->Destroy();

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(buf.get(), ResourceState::ShaderRead);
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("destroyed") != std::string::npos);
}

TEST_CASE("validation: commands after Finish are reported", "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Finish();
    encoder->BeginComputePass("late");  // after Finish
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("already finished") != std::string::npos);
}

TEST_CASE("validation: commands after pass End are reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    auto* pass = encoder->BeginComputePass("cp");
    pass->End();
    pass->SetPipeline(pipe.get());  // after End
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("already ended") != std::string::npos);
}

TEST_CASE("validation: drawing with no pipeline is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto color = MakeColorTarget(*device);

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    pass->Draw(3);  // no SetPipeline
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("no pipeline bound") != std::string::npos);
}

TEST_CASE("validation: an attachment lacking RenderTarget usage is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  // Sampled only -- not usable as an attachment.
  auto tex = device->CreateTexture({.width = 8, .height = 8,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled,
                                    .label = "sampled_only"});
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(tex.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = tex->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    if (pass) pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("RenderTarget") != std::string::npos);
}

TEST_CASE("validation: a depth texture in a color slot is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto depth = device->CreateTexture({.width = 8, .height = 8,
                                      .format = Format::Depth32Float,
                                      .usage = TextureUsage::DepthStencil,
                                      .label = "depth"});
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(depth.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = depth->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    if (pass) pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("depth format") != std::string::npos);
}

TEST_CASE("validation: an unended pass is reported at Finish",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->BeginComputePass("never_ended");
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("never ended") != std::string::npos);
}

TEST_CASE("validation: submitting an unfinished encoder is reported",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    device->Submit(*encoder);  // no Finish
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("not finished") != std::string::npos);
}

// --- Resource-state intent: the checks DX12 will lean on ---------------------

TEST_CASE("validation: reading a resource with no declared transition is reported",
          "[rhi][validation][state]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

  auto ubo = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "params"});
  auto ssbo = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::Storage, .label = "data"});
  auto tex = device->CreateTexture({.width = 4, .height = 4,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled,
                                    .label = "albedo"});
  auto samp = device->CreateSampler({});

  BindingTableDesc btd;
  btd.compute_pipeline = pipe.get();
  btd.label = "tbl";
  btd.entries = {
      {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
      {.slot = 1, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
      {.slot = 2, .kind = BindingKind::SampledTexture,
       .texture_view = tex->GetDefaultView()},
      {.slot = 3, .kind = BindingKind::Sampler, .sampler = samp.get()},
  };
  auto table = device->CreateBindingTable(btd);

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    // No Transition calls at all -- on Metal this would render correctly and
    // reveal nothing.
    pass->SetBindingTable(0, table.get());
    pass->Dispatch(1);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("Undefined") != std::string::npos);
  CHECK(observed->find("Transition") != std::string::npos);
}

TEST_CASE("validation: declaring the right states runs clean",
          "[rhi][validation][state]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

  auto ubo = device->CreateBuffer({.size = 64, .usage = BufferUsage::Uniform});
  auto ssbo = device->CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
  auto tex = device->CreateTexture({.width = 4, .height = 4,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled});
  auto samp = device->CreateSampler({});

  BindingTableDesc btd;
  btd.compute_pipeline = pipe.get();
  btd.entries = {
      {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
      {.slot = 1, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
      {.slot = 2, .kind = BindingKind::SampledTexture,
       .texture_view = tex->GetDefaultView()},
      {.slot = 3, .kind = BindingKind::Sampler, .sampler = samp.get()},
  };
  auto table = device->CreateBindingTable(btd);

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    // A uniform buffer and a sampled texture are read; the storage buffer is
    // written. Samplers carry no state.
    encoder->Transition(ubo.get(), ResourceState::ShaderRead);
    encoder->Transition(tex->GetDefaultView(), ResourceState::ShaderRead);
    encoder->Transition(ssbo.get(), ResourceState::ShaderWrite);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->SetBindingTable(0, table.get());
    pass->Dispatch(1);
    pass->End();
    encoder->Finish();
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
}

TEST_CASE("validation: an indirect buffer not in IndirectArg state is reported",
          "[rhi][validation][state]") {
  auto device = MakeValidated();
  auto vs = device->CreateShaderModule("", ShaderReflection{}, "vs");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = vs.get(), .color_formats = {Format::RGBA8Unorm}});

  auto color = MakeColorTarget(*device);
  auto index = device->CreateBuffer({.size = 64, .usage = BufferUsage::Index});
  auto args = device->CreateBuffer({.size = 32,
                                    .usage = BufferUsage::Indirect,
                                    .label = "args"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    // args is deliberately left Undefined.
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    pass->SetPipeline(pipe.get());
    pass->SetIndexBuffer(index.get(), IndexFormat::Uint32);
    pass->DrawIndexedIndirect(args.get(), 0);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("IndirectArg") != std::string::npos);
}

TEST_CASE("validation: a render target with no declared transition is reported",
          "[rhi][validation][state]") {
  auto device = MakeValidated();
  auto color = MakeColorTarget(*device, "untransitioned");
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    if (pass) pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  CHECK(observed->find("RenderTarget") != std::string::npos);
}

TEST_CASE("validation: several violations accumulate into one scope",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    (void)device->CreateBuffer({.size = 0, .usage = BufferUsage::None,
                                .label = "bad"});
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Finish();
    encoder->Finish();  // second Finish
  });
  REQUIRE(observed.has_value());
  // Accumulating rather than throwing is the point: one run surfaces every
  // problem, which is what makes the layer usable while porting.
  CHECK(observed->find(';') != std::string::npos);
}

TEST_CASE("validation: a device without validation observes nothing",
          "[rhi][validation]") {
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = false});
  REQUIRE(device);
  CHECK_FALSE(device->IsValidationEnabled());
  auto observed = Observe(*device, [&] {
    // Provoke something the validated device would report.
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Finish();
    encoder->Finish();
  });
  // nullopt here means "no validation ran", not "everything was fine" --
  // IsValidationEnabled() is how a caller tells those apart.
  CHECK_FALSE(observed.has_value());
}
