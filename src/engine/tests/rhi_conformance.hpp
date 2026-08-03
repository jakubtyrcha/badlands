#pragma once

// Behavioural assertions every RHI backend must satisfy, written once and run
// against each backend's suite.
//
// This is the mechanism that makes "every feature is tested at RHI level" hold
// as backends are added, rather than decaying into a Metal-only suite plus a
// Null-only suite that drift apart. `badlands_rhi_tests` runs this list against
// the Null backend with no GPU; `badlands_rhi_metal_tests` runs the SAME list
// against Metal and adds readback-based correctness on top.
//
// Assertions that can only be made against a recording backend are guarded on
// `GetCommandLog()` returning non-null, so a case still exercises the code path
// on Metal even when it cannot inspect the result.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "engine/rhi/null/null_rhi.hpp"
#include "engine/rhi/rhi_device.hpp"

namespace badlands::rhi::test {

// A reflection covering one binding of every kind at group 0, plus a compute
// entry point. Every kind is declared deliberately: the validation decorator
// checks bound slots against reflection, so a test that binds a slot the
// reflection never mentions would start failing the moment validation lands.
inline ShaderReflection MakeTestReflection(const char* entry = "main",
                                           ShaderStage stage = ShaderStage::Compute) {
  ShaderReflection r;
  r.bindings.push_back({.group = 0, .slot = 0, .name = "params",
                        .kind = BindingKind::UniformBuffer});
  r.bindings.push_back({.group = 0, .slot = 1, .name = "data",
                        .kind = BindingKind::StorageBuffer});
  r.bindings.push_back({.group = 0, .slot = 2, .name = "albedo",
                        .kind = BindingKind::SampledTexture});
  r.bindings.push_back({.group = 0, .slot = 3, .name = "albedo_sampler",
                        .kind = BindingKind::Sampler});
  r.uniform_blocks.push_back({.group = 0, .slot = 0, .name = "params",
                              .members = {{"scale", 0, 4, UniformType::Float}},
                              .total_size = 16});
  ReflectedEntryPoint ep;
  ep.name = entry;
  ep.stage = stage;
  ep.workgroup_size[0] = 64;
  r.entry_points.push_back(ep);
  return r;
}

inline std::span<const uint8_t> AsBytes(const std::vector<uint8_t>& v) {
  return {v.data(), v.size()};
}

// Every check that asserts on the command log must start from a clean one.
// The log lives on the device, so checks sharing a device would otherwise see
// each other's commands and count them -- which is exactly what the aggregate
// RunAllConformanceChecks caught the first time it ran. No-op on backends that
// do not record.
inline void ResetLog(IRhiDevice& device) {
  if (auto* log = null::GetCommandLog(device)) log->Clear();
}

// --- Resources --------------------------------------------------------------

inline void CheckBufferRoundTrip(IRhiDevice& device) {
  auto buf = device.CreateBuffer({.size = 256,
                                  .usage = BufferUsage::CopyDst |
                                           BufferUsage::CopySrc |
                                           BufferUsage::MapRead,
                                  .label = "roundtrip"});
  REQUIRE(buf);
  CHECK(buf->GetSize() == 256);

  std::vector<uint8_t> in(64);
  for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 3 + 1);
  buf->Write(0, AsBytes(in));

  std::vector<uint8_t> out(64, 0);
  REQUIRE(buf->Read(0, out));
  CHECK(out == in);
}

inline void CheckBufferBoundsAreRefused(IRhiDevice& device) {
  auto buf = device.CreateBuffer(
      {.size = 16, .usage = BufferUsage::CopyDst | BufferUsage::MapRead});
  std::vector<uint8_t> out(32, 0);
  // Reading past the end fails rather than reading garbage.
  CHECK_FALSE(buf->Read(0, out));
}

inline void CheckDestroyIsObservableAndIdempotent(IRhiDevice& device) {
  auto buf = device.CreateBuffer({.size = 16, .usage = BufferUsage::CopyDst});
  CHECK_FALSE(buf->IsDestroyed());
  buf->Destroy();
  CHECK(buf->IsDestroyed());
  buf->Destroy();  // idempotent
  CHECK(buf->IsDestroyed());
}

inline void CheckTextureCreationAndViews(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 32,
                                   .height = 16,
                                   .array_layers = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled |
                                            TextureUsage::CopyDst,
                                   .label = "layers"});
  REQUIRE(tex);
  CHECK(tex->GetWidth() == 32);
  CHECK(tex->GetHeight() == 16);
  CHECK(tex->GetArrayLayers() == 4);
  CHECK(tex->GetFormat() == Format::RGBA8Unorm);

  auto* v0 = tex->GetDefaultView();
  REQUIRE(v0 != nullptr);
  CHECK(v0->GetTexture() == tex.get());
  CHECK(v0->GetFormat() == Format::RGBA8Unorm);
  // Repeated default views are stable, so callers can compare by pointer.
  CHECK(tex->GetDefaultView() == v0);
}

// --- Pipelines and binding --------------------------------------------------

inline void CheckComputePipelineReportsWorkgroupSize(IRhiDevice& device) {
  auto module = device.CreateShaderModule("/* test */", MakeTestReflection(),
                                          "compute_module");
  REQUIRE(module);
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "main", .label = "compute"});
  REQUIRE(pipe);

  uint32_t wg[3] = {0, 0, 0};
  pipe->GetWorkgroupSize(wg);
  CHECK(wg[0] == 64);
  CHECK(wg[1] == 1);
  CHECK(wg[2] == 1);
}

inline void CheckReflectionLookupByName(IRhiDevice& device) {
  auto module = device.CreateShaderModule("", MakeTestReflection(), "m");
  const ShaderReflection& r = module->GetReflection();
  // Name lookup is the hook the render graph's auto-binding attaches to.
  const auto* b = r.FindBinding("data");
  REQUIRE(b != nullptr);
  CHECK(b->kind == BindingKind::StorageBuffer);
  CHECK(b->slot == 1);
  CHECK(r.FindBinding("nope") == nullptr);

  const auto* ub = r.FindUniformBlock("params");
  REQUIRE(ub != nullptr);
  CHECK(ub->total_size == 16);
  REQUIRE(ub->members.size() == 1);
  CHECK(ub->members[0].name == "scale");
}

inline void CheckBindingTableAcceptsEveryKind(IRhiDevice& device) {
  auto module = device.CreateShaderModule("", MakeTestReflection(), "m");
  auto pipe = device.CreateComputePipeline({.shader = module.get()});

  auto ubo = device.CreateBuffer({.size = 64, .usage = BufferUsage::Uniform});
  auto ssbo = device.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled});
  auto samp = device.CreateSampler({});

  BindingTableDesc d;
  d.compute_pipeline = pipe.get();
  d.group = 0;
  d.entries = {
      {.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = ubo.get()},
      {.slot = 1, .kind = BindingKind::StorageBuffer, .buffer = ssbo.get()},
      {.slot = 2, .kind = BindingKind::SampledTexture,
       .texture_view = tex->GetDefaultView()},
      {.slot = 3, .kind = BindingKind::Sampler, .sampler = samp.get()},
  };
  auto table = device.CreateBindingTable(d);
  REQUIRE(table);
  CHECK(table->GetGroup() == 0);
}

// --- Command recording ------------------------------------------------------

inline void CheckEncoderAndPassLifecycle(IRhiDevice& device) {
  auto encoder = device.CreateCommandEncoder("lifecycle");
  REQUIRE(encoder);
  CHECK_FALSE(encoder->IsFinished());

  auto* pass = encoder->BeginComputePass("cp");
  REQUIRE(pass != nullptr);
  CHECK_FALSE(pass->IsEnded());
  pass->End();
  CHECK(pass->IsEnded());
  pass->End();  // idempotent
  CHECK(pass->IsEnded());

  encoder->Finish();
  CHECK(encoder->IsFinished());
  device.Submit(*encoder);
  device.WaitIdle();
}

inline void CheckRenderPassAttachmentsAreRecorded(IRhiDevice& device) {
  ResetLog(device);
  auto color = device.CreateTexture({.width = 8, .height = 8,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::RenderTarget});
  auto depth = device.CreateTexture({.width = 8, .height = 8,
                                     .format = Format::Depth32Float,
                                     .usage = TextureUsage::DepthStencil});

  auto encoder = device.CreateCommandEncoder("rp");
  // Declared even though Metal would not need it: these checks double as the
  // worked example of correct usage, and rhi_validation_tests asserts the whole
  // conformance list runs without tripping a single check.
  encoder->Transition(color.get(), ResourceState::RenderTarget);
  encoder->Transition(depth.get(), ResourceState::DepthWrite);

  RenderPassDesc desc;
  desc.label = "gbuffer";
  desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                    .load_op = LoadOp::Clear,
                                    .store_op = StoreOp::Store});
  // Reversed-Z: depth clears to 0.0 (far), not 1.0.
  desc.depth_attachment = {.view = depth->GetDefaultView(),
                           .load_op = LoadOp::Clear,
                           .store_op = StoreOp::Store,
                           .clear_depth = 0.0f};
  auto* pass = encoder->BeginRenderPass(desc);
  REQUIRE(pass != nullptr);
  pass->End();
  encoder->Finish();

  if (auto* log = null::GetCommandLog(device)) {
    const auto* begin =
        log->Find(null::RecordedCommand::Kind::BeginRenderPass);
    REQUIRE(begin != nullptr);
    CHECK(begin->color_attachment_count == 1);
    CHECK(begin->has_depth);
    CHECK(begin->first_color_load == LoadOp::Clear);
    CHECK(begin->first_color_store == StoreOp::Store);
    CHECK(log->Count(null::RecordedCommand::Kind::EndRenderPass) == 1);
  }
}

// The GPU-driven case, made testable without a GPU: seed the indirect-args
// buffer, issue the draw, and assert on the args that were actually consumed.
// On Metal the same sequence runs but the assertion is skipped -- there a
// compute dispatch writes the count and readback proves the result instead.
inline void CheckIndirectDrawReadsArgsFromBuffer(IRhiDevice& device) {
  ResetLog(device);
  auto args = device.CreateBuffer({.size = sizeof(DrawIndexedIndirectArgs),
                                   .usage = BufferUsage::Indirect |
                                            BufferUsage::CopyDst,
                                   .label = "indirect_args"});
  const DrawIndexedIndirectArgs seeded{
      .index_count = 96, .instance_count = 7, .first_index = 12,
      .base_vertex = 3, .first_instance = 1};
  std::vector<uint8_t> bytes(sizeof(seeded));
  std::memcpy(bytes.data(), &seeded, sizeof(seeded));
  args->Write(0, AsBytes(bytes));

  auto color = device.CreateTexture({.width = 4, .height = 4,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::RenderTarget});
  auto index = device.CreateBuffer(
      {.size = 256, .usage = BufferUsage::Index, .label = "indices"});
  auto vs = device.CreateShaderModule("", ShaderReflection{}, "vs");
  auto pipe = device.CreateRenderPipeline(
      {.vertex_shader = vs.get(), .color_formats = {Format::RGBA8Unorm},
       .label = "indirect_pipe"});

  auto encoder = device.CreateCommandEncoder("indirect");
  encoder->Transition(args.get(), ResourceState::IndirectArg);
  encoder->Transition(color.get(), ResourceState::RenderTarget);

  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView()});
  auto* pass = encoder->BeginRenderPass(desc);
  pass->SetPipeline(pipe.get());
  pass->SetIndexBuffer(index.get(), IndexFormat::Uint32);
  pass->DrawIndexedIndirect(args.get(), 0);
  pass->End();
  encoder->Finish();

  if (auto* log = null::GetCommandLog(device)) {
    const auto* draw =
        log->Find(null::RecordedCommand::Kind::DrawIndexedIndirect);
    REQUIRE(draw != nullptr);
    CHECK(draw->draw_args.index_count == 96);
    CHECK(draw->draw_args.instance_count == 7);
    CHECK(draw->draw_args.first_index == 12);
    CHECK(draw->draw_args.base_vertex == 3);
    CHECK(draw->draw_args.first_instance == 1);
  }
}

inline void CheckBufferCopyMovesBytes(IRhiDevice& device) {
  auto src = device.CreateBuffer(
      {.size = 32, .usage = BufferUsage::CopySrc | BufferUsage::CopyDst});
  auto dst = device.CreateBuffer({.size = 32, .usage = BufferUsage::CopyDst |
                                                      BufferUsage::MapRead});
  std::vector<uint8_t> in(32);
  for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(0xA0 + i);
  src->Write(0, AsBytes(in));

  auto encoder = device.CreateCommandEncoder("copy");
  encoder->Transition(src.get(), ResourceState::CopySrc);
  encoder->Transition(dst.get(), ResourceState::CopyDst);
  encoder->CopyBufferToBuffer(src.get(), 0, dst.get(), 0, 32);
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  std::vector<uint8_t> out(32, 0);
  REQUIRE(dst->Read(0, out));
  CHECK(out == in);
}

inline void CheckTransitionsAreRecorded(IRhiDevice& device) {
  ResetLog(device);
  auto buf = device.CreateBuffer({.size = 16, .usage = BufferUsage::Storage});
  auto encoder = device.CreateCommandEncoder("transitions");

  const ResourceTransition batch[] = {
      {buf.get(), ResourceState::ShaderWrite},
      {buf.get(), ResourceState::ShaderRead},
  };
  encoder->TransitionMany(batch);
  encoder->Finish();

  if (auto* log = null::GetCommandLog(device)) {
    CHECK(log->Count(null::RecordedCommand::Kind::Transition) == 2);
    const auto* first = log->Find(null::RecordedCommand::Kind::Transition, 0);
    const auto* second = log->Find(null::RecordedCommand::Kind::Transition, 1);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->state == ResourceState::ShaderWrite);
    CHECK(second->state == ResourceState::ShaderRead);
  }
}

inline void CheckComputeDispatchIsRecorded(IRhiDevice& device) {
  ResetLog(device);
  auto module = device.CreateShaderModule("", MakeTestReflection(), "m");
  auto pipe = device.CreateComputePipeline({.shader = module.get()});
  auto encoder = device.CreateCommandEncoder("dispatch");
  auto* pass = encoder->BeginComputePass("classify");
  pass->SetPipeline(pipe.get());
  pass->Dispatch(8, 2, 1);
  pass->End();
  encoder->Finish();

  if (auto* log = null::GetCommandLog(device)) {
    const auto* d = log->Find(null::RecordedCommand::Kind::Dispatch);
    REQUIRE(d != nullptr);
    CHECK(d->dispatch[0] == 8);
    CHECK(d->dispatch[1] == 2);
    CHECK(d->dispatch[2] == 1);
  }
}

// --- The whole list ---------------------------------------------------------

// Every backend runs exactly this. Adding a feature means adding a case here,
// which is what keeps backends from diverging.
inline void RunAllConformanceChecks(IRhiDevice& device) {
  CheckBufferRoundTrip(device);
  CheckBufferBoundsAreRefused(device);
  CheckDestroyIsObservableAndIdempotent(device);
  CheckTextureCreationAndViews(device);
  CheckComputePipelineReportsWorkgroupSize(device);
  CheckReflectionLookupByName(device);
  CheckBindingTableAcceptsEveryKind(device);
  CheckEncoderAndPassLifecycle(device);
  CheckRenderPassAttachmentsAreRecorded(device);
  CheckIndirectDrawReadsArgsFromBuffer(device);
  CheckBufferCopyMovesBytes(device);
  CheckTransitionsAreRecorded(device);
  CheckComputeDispatchIsRecorded(device);
}

}  // namespace badlands::rhi::test
