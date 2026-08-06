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

// Runs `fn` inside a validation scope and returns what it observed, or nullopt
// if the scope was clean.
//
// The REQUIRE is the point: EndValidationScope returns nullopt when NO check
// ran, and a test that reads that as "clean" asserts nothing at all. Having
// proved a report exists, this collapses it back to the optional<string> the
// cases below read, where nullopt now means clean and only clean.
template <typename Fn>
std::optional<std::string> Observe(IRhiDevice& device, Fn&& fn) {
  device.BeginValidationScope();
  fn();
  auto report = device.EndValidationScope();
  REQUIRE(report.has_value());
  if (report->IsClean()) return std::nullopt;
  return report->violations;
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

// --- Bindings must resolve, and must resolve against something --------------

TEST_CASE("validation: a slot the shader does not declare is reported",
          "[rhi][validation]") {
  // The reverse of the "declared but not bound" case above, and the direction
  // that was missing: an entry with no reflection behind it used to be
  // accepted here and then guessed at by the backend, landing the resource on
  // whatever the shader declared at that index.
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
    d.label = "extra";
    // Reflection declares slots 0..3. Bind all of them, plus a slot 7 that
    // exists nowhere in the shader.
    d.entries = {
        {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
        {.slot = 1, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
        {.slot = 2, .kind = BindingKind::SampledTexture,
         .texture_view = tex->GetDefaultView()},
        {.slot = 3, .kind = BindingKind::Sampler, .sampler = samp.get()},
        {.slot = 7, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
    };
    (void)device->CreateBindingTable(d);
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("slot 7") != std::string::npos);
  CHECK(observed->find("no such binding") != std::string::npos);
}

TEST_CASE("validation: binding before SetPipeline is reported",
          "[rhi][validation]") {
  // Harmless on Metal, wrong on DX12 (the root signature has to be set first),
  // and wrong everywhere in principle: bindings resolve against the bound
  // pipeline's reflection, so there is nothing to resolve against yet.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto set = rhitest::MakeFullBindingSet(*device, pipe.get(), "ordered");
  REQUIRE(set);

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    set.TransitionAll(*encoder);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetBindingTable(0, set.table.get());  // before SetPipeline
    pass->SetPipeline(pipe.get());
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("before any SetPipeline") != std::string::npos);
}

TEST_CASE("validation: the right order reports nothing", "[rhi][validation]") {
  // The paired green: same calls, correct order, silent. Without this the test
  // above would pass just as well against a check that fires unconditionally.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto set = rhitest::MakeFullBindingSet(*device, pipe.get(), "ordered");
  REQUIRE(set);

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    set.TransitionAll(*encoder);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->SetBindingTable(0, set.table.get());
    pass->End();
    encoder->Finish();
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
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

// --- Bounds Dawn used to check for us ---------------------------------------

TEST_CASE("validation: an out-of-bounds buffer copy is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto src = device->CreateBuffer({.size = 64,
                                   .usage = BufferUsage::CopySrc,
                                   .label = "src64"});
  auto dst = device->CreateBuffer({.size = 32,
                                   .usage = BufferUsage::CopyDst,
                                   .label = "dst32"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(src.get(), ResourceState::CopySrc);
    encoder->Transition(dst.get(), ResourceState::CopyDst);
    encoder->CopyBufferToBuffer(src.get(), 0, dst.get(), 0, 64);  // dst is 32
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("dst32") != std::string::npos);
  CHECK(observed->find("runs past") != std::string::npos);

  // And the copy did not happen: refuse, do not report-and-proceed (rule 3).
  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       CopyBufferToBuffer) == 0);
}

TEST_CASE("validation: a copy offset that would wrap is refused",
          "[rhi][validation]") {
  // `offset + size` overflows to a small number and passes a naive check.
  auto device = MakeValidated();
  auto src = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::CopySrc, .label = "wrapsrc"});
  auto dst = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::CopyDst, .label = "wrapdst"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(src.get(), ResourceState::CopySrc);
    encoder->Transition(dst.get(), ResourceState::CopyDst);
    encoder->CopyBufferToBuffer(src.get(), ~uint64_t(0) - 8, dst.get(), 0, 16);
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("runs past") != std::string::npos);
}

TEST_CASE("validation: a texture copy into too small a buffer is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto tex = device->CreateTexture({.width = 8, .height = 8,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::CopySrc,
                                    .label = "tex8"});
  // 8*8*4 = 256 bytes needed; give it 64.
  auto dst = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::CopyDst, .label = "tiny"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(tex.get(), ResourceState::CopySrc);
    encoder->Transition(dst.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(tex.get(), 0, 0, dst.get(), 0);
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("will not fit") != std::string::npos);
}

TEST_CASE("validation: a copy from a mip or layer that does not exist is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto tex = device->CreateTexture({.width = 8, .height = 8,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::CopySrc,
                                    .label = "flat"});
  auto dst = device->CreateBuffer(
      {.size = 4096, .usage = BufferUsage::CopyDst, .label = "big"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(tex.get(), ResourceState::CopySrc);
    encoder->Transition(dst.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(tex.get(), /*mip=*/4, 0, dst.get(), 0);
    encoder->CopyTextureToBuffer(tex.get(), 0, /*layer=*/3, dst.get(), 0);
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("mip 4") != std::string::npos);
  CHECK(observed->find("layer 3") != std::string::npos);
}

TEST_CASE("validation: an indexed draw past the end of its index buffer is refused",
          "[rhi][validation]") {
  // first_index became load-bearing when the backend started honouring it,
  // and nothing checked that the range it selects exists.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalGraphicsSource(device->GetBackend()),
      rhitest::MakeTestReflection("vs_main", ShaderStage::Vertex), "g");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "idx"});
  REQUIRE(pipe);
  auto color = MakeColorTarget(*device);
  // Room for 6 uint32 indices.
  auto ibuf = device->CreateBuffer(
      {.size = 24, .usage = BufferUsage::Index, .label = "six_indices"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    REQUIRE(pass != nullptr);
    pass->SetPipeline(pipe.get());
    pass->SetIndexBuffer(ibuf.get(), IndexFormat::Uint32);
    pass->DrawIndexed(/*index_count=*/3, 1, /*first_index=*/4);  // needs 7
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("six_indices") != std::string::npos);
  CHECK(observed->find("indices [4, 7)") != std::string::npos);
}

TEST_CASE("validation: an indexed draw at a wrapping offset is refused",
          "[rhi][validation]") {
  // `index_offset_ + count * stride` wraps to a small number and passes a
  // check it has to fail. This is the offset a caller lands on when it is
  // computed as an underflowing subtraction.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalGraphicsSource(device->GetBackend()),
      rhitest::MakeTestReflection("vs_main", ShaderStage::Vertex), "g");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "idx"});
  auto color = MakeColorTarget(*device);
  auto ibuf = device->CreateBuffer(
      {.size = 24, .usage = BufferUsage::Index, .label = "six_indices"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    REQUIRE(pass != nullptr);
    pass->SetPipeline(pipe.get());
    pass->SetIndexBuffer(ibuf.get(), IndexFormat::Uint32, ~uint64_t(0) - 8);
    pass->DrawIndexed(3);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("do not fit") != std::string::npos);
}

TEST_CASE("validation: an indirect draw past its argument buffer is refused",
          "[rhi][validation]") {
  // DrawIndexed gained a range check; its indirect sibling had none, on the
  // path the GPU-driven MVP actually uses. The ARGS live on the GPU and cannot
  // be checked here, but the OFFSET is a CPU-side value crossing the API.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalGraphicsSource(device->GetBackend()),
      rhitest::MakeTestReflection("vs_main", ShaderStage::Vertex), "g");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "ind"});
  auto color = MakeColorTarget(*device);
  auto ibuf = device->CreateBuffer(
      {.size = 24, .usage = BufferUsage::Index, .label = "indices"});
  auto args = device->CreateBuffer(
      {.size = sizeof(DrawIndexedIndirectArgs),
       .usage = BufferUsage::Indirect, .label = "args"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    encoder->Transition(args.get(), ResourceState::IndirectArg);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    pass->SetPipeline(pipe.get());
    pass->SetIndexBuffer(ibuf.get(), IndexFormat::Uint32);
    // One struct's worth of room, asked to read a struct starting 4 bytes in.
    pass->DrawIndexedIndirect(args.get(), 4);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("do not fit") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DrawIndexedIndirect) == 0);
}

TEST_CASE("validation: an indirect draw with no index buffer is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalGraphicsSource(device->GetBackend()),
      rhitest::MakeTestReflection("vs_main", ShaderStage::Vertex), "g");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "ind"});
  auto color = MakeColorTarget(*device);
  auto args = device->CreateBuffer(
      {.size = sizeof(DrawIndexedIndirectArgs),
       .usage = BufferUsage::Indirect, .label = "args"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    encoder->Transition(args.get(), ResourceState::IndirectArg);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    pass->SetPipeline(pipe.get());
    pass->DrawIndexedIndirect(args.get(), 0);  // no SetIndexBuffer
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("no index buffer bound") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DrawIndexedIndirect) == 0);
}

TEST_CASE("validation: a zero indirect dispatch is NOT refused",
          "[rhi][validation]") {
  // The asymmetry, asserted where it lives. Dispatch(0,1,1) is refused because
  // Metal's debug layer aborts on it; DispatchIndirect cannot see the counts,
  // which are in GPU memory, and a zero-group indirect dispatch is a legal
  // no-op that an empty cull produces every frame.
  //
  // The conformance case for this runs on an UNVALIDATED device, so it could
  // never have caught a decorator that refused zero -- which is exactly what
  // its red proof revealed.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto args = device->CreateBuffer(
      {.size = sizeof(DispatchIndirectArgs),
       .usage = BufferUsage::Indirect | BufferUsage::CopyDst,
       .label = "zero_args"});
  const DispatchIndirectArgs zero{};
  args->Write(0, {reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(args.get(), ResourceState::IndirectArg);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->DispatchIndirect(args.get(), 0);
    pass->End();
    encoder->Finish();
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());

  // And it actually reached the backend.
  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DispatchIndirect) == 1);
}

TEST_CASE("validation: an unaligned indirect offset is refused",
          "[rhi][validation]") {
  // Metal requires an indirect buffer offset to be 4-byte aligned. Nothing
  // checked it before, for dispatches OR draws.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  auto args = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::Indirect, .label = "args"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(args.get(), ResourceState::IndirectArg);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->DispatchIndirect(args.get(), 2);  // aligned to 2, not 4
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("not 4-byte aligned") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DispatchIndirect) == 0);
}

TEST_CASE("validation: a zero-workgroup dispatch is refused",
          "[rhi][validation]") {
  // Metal's debug layer aborts the process on a zero-sized dispatch, so
  // reporting and then forwarding turned a diagnosable mistake into a dead
  // test binary.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalComputeSource(device->GetBackend()),
      rhitest::MakeTestReflection(), "m");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->Dispatch(0, 1, 1);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("zero workgroup count") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::Dispatch) == 0);
}

TEST_CASE("validation: an in-bounds indexed draw is not refused",
          "[rhi][validation]") {
  // The paired green: exactly the range that fits reports nothing, so the
  // case above cannot pass against a check that always fires.
  auto device = MakeValidated();
  auto module = device->CreateShaderModule(
      rhitest::MinimalGraphicsSource(device->GetBackend()),
      rhitest::MakeTestReflection("vs_main", ShaderStage::Vertex), "g");
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "idx"});
  auto color = MakeColorTarget(*device);
  auto ibuf = device->CreateBuffer(
      {.size = 24, .usage = BufferUsage::Index, .label = "six_indices"});

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(color.get(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = color->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(desc);
    pass->SetPipeline(pipe.get());
    pass->SetIndexBuffer(ibuf.get(), IndexFormat::Uint32);
    pass->DrawIndexed(/*index_count=*/3, 1, /*first_index=*/3);  // needs 6
    pass->End();
    encoder->Finish();
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
}

TEST_CASE("validation: a foreign encoder is refused rather than forwarded",
          "[rhi][validation]") {
  // Reporting and then forwarding converted a diagnosable mistake into
  // undefined behaviour: the backend's Submit static_casts to its own encoder
  // type, so a foreign one is a wrong-type cast (rule 3).
  auto validated = MakeValidated();
  auto other = CreateDevice({.backend = BackendKind::Null,
                             .enable_validation = false, .label = "other"});
  REQUIRE(validated);
  REQUIRE(other);

  auto foreign = other->CreateCommandEncoder("foreign");
  foreign->Finish();

  auto observed = Observe(*validated, [&] { validated->Submit(*foreign); });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("did not come from this device") != std::string::npos);
  CHECK(observed->find("refusing") != std::string::npos);

  // The submission did not reach the inner device.
  auto* log = badlands::rhi::null::GetCommandLog(*validated);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::Submit) == 0);
}

TEST_CASE("validation: an unfinished encoder is refused rather than submitted",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("unfinished");
    device->Submit(*encoder);  // never Finish()ed
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("was not finished") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::Submit) == 0);
}

// --- Dynamic offsets --------------------------------------------------------

namespace {

// A compute table whose slot 0 takes a dynamic offset, plus the resources it
// needs kept alive.
struct DynamicSet {
  ComputePipelinePtr pipe;
  BufferPtr ubo, ssbo;
  TexturePtr tex;
  SamplerPtr samp;
  BindingTablePtr table;

  void TransitionAll(ICommandEncoder& e) const {
    e.Transition(ubo.get(), ResourceState::ShaderRead);
    e.Transition(ssbo.get(), ResourceState::ShaderWrite);
    e.Transition(tex.get(), ResourceState::ShaderRead);
  }
};

DynamicSet MakeDynamicSet(IRhiDevice& d) {
  DynamicSet s;
  s.pipe = rhitest::MakeTestPipeline(d);
  s.ubo = d.CreateBuffer(
      {.size = 1024, .usage = BufferUsage::Uniform, .label = "dyn_ubo"});
  s.ssbo = d.CreateBuffer({.size = 1024, .usage = BufferUsage::Storage});
  s.tex = d.CreateTexture({.width = 4, .height = 4,
                           .format = Format::RGBA8Unorm,
                           .usage = TextureUsage::Sampled});
  s.samp = d.CreateSampler({});
  s.table = d.CreateBindingTable(
      {.compute_pipeline = s.pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = s.ubo.get(), .dynamic_offset = true},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = s.ssbo.get()},
                   {.slot = 2, .kind = BindingKind::SampledTexture,
                    .texture_view = s.tex->GetDefaultView()},
                   {.slot = 3, .kind = BindingKind::Sampler,
                    .sampler = s.samp.get()}},
       .label = "dyn"});
  return s;
}

// Binds `s` with the given offsets inside a validation scope.
template <typename Fn>
void WithDynamicPass(IRhiDevice& d, DynamicSet& s, Fn&& bind) {
  auto encoder = d.CreateCommandEncoder("e");
  encoder->Transition(s.ubo.get(), ResourceState::ShaderRead);
  encoder->Transition(s.ssbo.get(), ResourceState::ShaderWrite);
  encoder->Transition(s.tex.get(), ResourceState::ShaderRead);
  auto* pass = encoder->BeginComputePass("cp");
  pass->SetPipeline(s.pipe.get());
  bind(pass);
  pass->End();
  encoder->Finish();
}

}  // namespace

TEST_CASE("validation: a missing dynamic offset is refused",
          "[rhi][validation]") {
  // Too few offsets shifts every later binding onto the wrong slice, so there
  // is no partial-credit way to proceed.
  auto device = MakeValidated();
  auto s = MakeDynamicSet(*device);
  REQUIRE(s.table);

  auto observed = Observe(*device, [&] {
    WithDynamicPass(*device, s, [&](IComputePass* pass) {
      pass->SetBindingTable(0, s.table.get());  // declares one, supplies none
    });
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("dynamic offset") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                       SetBindingTable) == 0);
}

TEST_CASE("validation: an unaligned dynamic offset is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto s = MakeDynamicSet(*device);
  REQUIRE(s.table);
  const uint32_t bad[1] = {1};  // never a multiple of any real alignment

  auto observed = Observe(*device, [&] {
    WithDynamicPass(*device, s, [&](IComputePass* pass) {
      pass->SetBindingTable(0, s.table.get(), bad);
    });
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("not a multiple") != std::string::npos);
}

TEST_CASE("validation: a dynamic offset past the buffer is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto s = MakeDynamicSet(*device);
  REQUIRE(s.table);
  // Aligned, but well past the 1024-byte buffer.
  const uint32_t far[1] = {4096};

  auto observed = Observe(*device, [&] {
    WithDynamicPass(*device, s, [&](IComputePass* pass) {
      pass->SetBindingTable(0, s.table.get(), far);
    });
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("would bind at") != std::string::npos);
}

TEST_CASE("validation: a refused bind also refuses the dispatch",
          "[rhi][validation]") {
  // Refusing the BIND alone leaves whatever was bound at that group in place,
  // so the dispatch reads the PREVIOUS table's resources -- a different wrong
  // answer than the release build gives, which is the worst kind.
  auto device = MakeValidated();
  auto s = MakeDynamicSet(*device);
  REQUIRE(s.table);
  const uint32_t bad[1] = {1};  // unaligned: the bind will be refused

  auto observed = Observe(*device, [&] {
    auto encoder = device->CreateCommandEncoder("e");
    s.TransitionAll(*encoder);
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(s.pipe.get());
    pass->SetBindingTable(0, s.table.get(), bad);
    pass->Dispatch(1);
    pass->End();
    encoder->Finish();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("wrong resources") != std::string::npos);

  auto* log = badlands::rhi::null::GetCommandLog(*device);
  REQUIRE(log != nullptr);
  CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::Dispatch) == 0);
}

TEST_CASE("validation: a correct dynamic offset reports nothing",
          "[rhi][validation]") {
  // The paired green: the three refusals above would all pass against a check
  // that fires unconditionally.
  auto device = MakeValidated();
  auto s = MakeDynamicSet(*device);
  REQUIRE(s.table);
  const uint32_t good[1] = {256};  // aligned for every backend, inside 1024

  auto observed = Observe(*device, [&] {
    WithDynamicPass(*device, s, [&](IComputePass* pass) {
      pass->SetBindingTable(0, s.table.get(), good);
    });
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
}

// --- Swapchain misuse -------------------------------------------------------

TEST_CASE("validation: acquiring twice without a present is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 32, .height = 32, .label = "sc"});
  REQUIRE(sc);

  auto observed = Observe(*device, [&] {
    device->BeginFrame();
    auto a = sc->Acquire();
    CHECK(a.status == AcquireStatus::Ok);
    auto b = sc->Acquire();  // still holding the first
    CHECK(b.status == AcquireStatus::Skip);
    CHECK(b.view == nullptr);
    sc->Present();
    device->EndFrame();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("not been presented") != std::string::npos);
}

TEST_CASE("validation: presenting without an acquire is refused",
          "[rhi][validation]") {
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 32, .height = 32, .label = "sc"});
  REQUIRE(sc);

  auto observed = Observe(*device, [&] {
    device->BeginFrame();
    sc->Present();  // nothing acquired
    device->EndFrame();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("nothing was acquired") != std::string::npos);
  // And the inner swapchain never saw it.
  CHECK(badlands::rhi::null::PresentCount(*sc) == 0);
}

TEST_CASE("validation: presenting twice is refused", "[rhi][validation]") {
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 32, .height = 32, .label = "sc"});
  REQUIRE(sc);

  auto observed = Observe(*device, [&] {
    device->BeginFrame();
    REQUIRE(sc->Acquire().status == AcquireStatus::Ok);
    sc->Present();
    sc->Present();  // the second has nothing to present
    device->EndFrame();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(badlands::rhi::null::PresentCount(*sc) == 1);
}

TEST_CASE("validation: rendering into a pre-resize backbuffer is refused",
          "[rhi][validation]") {
  // The resize hazard. The stale view is the RIGHT object at the WRONG size,
  // so nothing about it looks broken until the frame comes out stretched --
  // or, on Metal, until an attachment turns out to be nil.
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 64, .height = 64, .label = "sc"});
  REQUIRE(sc);

  device->BeginFrame();
  auto stale = sc->Acquire();
  REQUIRE(stale.status == AcquireStatus::Ok);
  sc->Present();
  device->EndFrame();

  auto observed = Observe(*device, [&] {
    device->BeginFrame();
    sc->Resize(128, 128);
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(stale.view->GetTexture(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = stale.view});
    auto* pass = encoder->BeginRenderPass(desc);
    CHECK(pass == nullptr);  // refused, not merely reported
    encoder->Finish();
    device->EndFrame();
  });
  REQUIRE(observed.has_value());
  INFO(*observed);
  CHECK(observed->find("before a resize") != std::string::npos);
}

TEST_CASE("validation: a freshly acquired backbuffer is fine after a resize",
          "[rhi][validation]") {
  // The paired green: the case above must not be satisfied by refusing every
  // swapchain view.
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 64, .height = 64, .label = "sc"});
  REQUIRE(sc);

  auto observed = Observe(*device, [&] {
    device->BeginFrame();
    sc->Resize(128, 128);
    auto fresh = sc->Acquire();
    REQUIRE(fresh.status == AcquireStatus::Ok);
    auto encoder = device->CreateCommandEncoder("e");
    encoder->Transition(fresh.view->GetTexture(), ResourceState::RenderTarget);
    RenderPassDesc desc;
    desc.color_attachments.push_back({.view = fresh.view});
    auto* pass = encoder->BeginRenderPass(desc);
    CHECK(pass != nullptr);
    pass->End();
    encoder->Finish();
    sc->Present();
    device->EndFrame();
  });
  INFO(observed.value_or("<clean>"));
  CHECK_FALSE(observed.has_value());
}

TEST_CASE("validation: an injected Lost is reported as Lost, not Skip",
          "[rhi][validation]") {
  // Skip means "try again next frame"; Lost means "recreate the surface".
  // Telling a caller to retry a surface that will never come back is an
  // infinite loop of black frames.
  auto device = MakeValidated();
  auto sc = device->CreateSwapchain({.width = 32, .height = 32, .label = "sc"});
  REQUIRE(sc);

  badlands::rhi::null::SetSwapchainFault(
      *sc, badlands::rhi::null::SwapchainFault::Lost);
  device->BeginFrame();
  auto lost = sc->Acquire();
  CHECK(lost.status == AcquireStatus::Lost);
  CHECK(lost.view == nullptr);
  device->EndFrame();

  badlands::rhi::null::SetSwapchainFault(
      *sc, badlands::rhi::null::SwapchainFault::Skip);
  device->BeginFrame();
  auto skip = sc->Acquire();
  CHECK(skip.status == AcquireStatus::Skip);
  CHECK(skip.view == nullptr);
  device->EndFrame();

  // A skipped frame must not have presented anything.
  CHECK(badlands::rhi::null::PresentCount(*sc) == 0);
  device->WaitIdle();
}

TEST_CASE("validation: unchecked is distinguishable from clean",
          "[rhi][validation]") {
  // The whole reason EndValidationScope returns a report rather than an
  // optional<string>. Before, "nothing was checked" and "everything was fine"
  // were the same value, and a caller had to ask IsValidationEnabled()
  // separately to tell them apart -- which is exactly the workaround rule 5
  // says to replace with a type that cannot express the confusion.
  auto provoke = [](IRhiDevice& d) {
    auto encoder = d.CreateCommandEncoder("e");
    encoder->Finish();
    encoder->Finish();  // second Finish: the validated device reports this
  };

  // Validation off: no report at all.
  auto bare = CreateDevice({.backend = BackendKind::Null,
                            .enable_validation = false});
  REQUIRE(bare);
  CHECK_FALSE(bare->IsValidationEnabled());
  bare->BeginValidationScope();
  provoke(*bare);
  CHECK_FALSE(bare->EndValidationScope().has_value());

  // Validation on, same calls: a report, and a dirty one.
  auto checked = MakeValidated();
  REQUIRE(checked);
  checked->BeginValidationScope();
  provoke(*checked);
  auto dirty = checked->EndValidationScope();
  REQUIRE(dirty.has_value());
  CHECK_FALSE(dirty->IsClean());

  // Validation on, nothing provoked: a report, and a clean one. This is the
  // value that used to be indistinguishable from the bare device's above.
  checked->BeginValidationScope();
  auto clean = checked->EndValidationScope();
  REQUIRE(clean.has_value());
  CHECK(clean->IsClean());
  CHECK(clean->violations.empty());
}

TEST_CASE("validation: ending a scope that never began yields no report",
          "[rhi][validation]") {
  // Also nullopt, and correctly so: there is nothing to report on. A caller
  // that mismatches Begin/End learns it here rather than reading a clean run.
  auto device = MakeValidated();
  REQUIRE(device);
  CHECK_FALSE(device->EndValidationScope().has_value());
}
