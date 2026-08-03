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

// Minimal shader source per backend.
//
// Shader source is TARGET-NATIVE by contract -- the RHI never invokes a
// compiler -- so a shared behavioural list cannot use one string for every
// backend. Null ignores the source entirely; Metal must actually compile it.
// DX12 adds an HLSL arm here rather than anywhere else.
//
// Entry points are named cs_/vs_/fs_main rather than `main`: Slang renames a
// Metal entry called `main` to `main_0`, and matching that convention here
// keeps the tests from encoding a trap the real shaders avoid.
inline const char* MinimalComputeSource(BackendKind backend) {
  switch (backend) {
    case BackendKind::Metal:
      return R"(
#include <metal_stdlib>
using namespace metal;
// uint3, not uint: CheckComputeDispatchIsRecorded dispatches a 2-D grid,
// and a scalar thread id makes that an invalid call that only
// MTL_DEBUG_LAYER=1 reports -- it aborted the suite the first time the layer
// was switched on.
kernel void cs_main(uint3 gid [[thread_position_in_grid]]) {}
)";
    case BackendKind::Null:
      return "";
  }
  return "";
}

inline const char* MinimalGraphicsSource(BackendKind backend) {
  switch (backend) {
    case BackendKind::Metal:
      return R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs_main(uint vid [[vertex_id]]) {
  VOut o; o.pos = float4(0.0, 0.0, 0.0, 1.0); return o;
}
fragment float4 fs_main() { return float4(1.0); }
)";
    case BackendKind::Null:
      return "";
  }
  return "";
}

// Draws a real triangle by pulling positions from a storage buffer via the
// vertex id -- the MVP's vertex model, and the only way to exercise DrawIndexed
// without vertex input layouts.
inline const char* PullingGraphicsSource(BackendKind backend) {
  switch (backend) {
    case BackendKind::Metal:
      return R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs_main(uint vid [[vertex_id]],
                    device const float4* verts [[buffer(0)]]) {
  VOut o; o.pos = verts[vid]; return o;
}
fragment float4 fs_main() { return float4(0.0, 1.0, 0.0, 1.0); }
)";
    case BackendKind::Null:
      return "";
  }
  return "";
}

// Samples a texture across a fullscreen triangle, so an upload can be proven to
// have reached the GPU rather than merely not crashed.
inline const char* SamplingGraphicsSource(BackendKind backend) {
  switch (backend) {
    case BackendKind::Metal:
      return R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_main(uint vid [[vertex_id]]) {
  float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
  VOut o; o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0); o.uv = uv; return o;
}
fragment float4 fs_main(VOut i [[stage_in]],
                        texture2d<float> tex [[texture(0)]],
                        sampler samp [[sampler(0)]]) {
  return tex.sample(samp, i.uv);
}
)";
    case BackendKind::Null:
      return "";
  }
  return "";
}

// Whether this backend actually rasterizes, so readback assertions mean
// something. Null records commands but produces no pixels.
//
// Deliberately keyed on the backend rather than on "is there a command log":
// per this directory's rule 6, anything Null cannot observe still needs a
// real assertion somewhere, and making that explicit keeps the gap visible.
inline bool ProducesPixels(IRhiDevice& device) {
  return device.GetBackend() != BackendKind::Null;
}

// Reads the centre texel of an RGBA8 target that has been copied into `buf`.
struct Rgba { uint8_t r, g, b, a; };
inline Rgba CentrePixel(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
  const size_t o = (size_t(h / 2) * w + w / 2) * 4;
  return {px[o], px[o + 1], px[o + 2], px[o + 3]};
}

// A reflection covering one binding of every kind at group 0, plus a compute
// entry point. Every kind is declared deliberately: the validation decorator
// checks bound slots against reflection, so a test that binds a slot the
// reflection never mentions would start failing the moment validation lands.
//
// `location.index` mirrors the slot, which is what Slang reports for plain
// globals and what the Metal backend binds at.
inline ShaderReflection MakeTestReflection(const char* entry = "cs_main",
                                           ShaderStage stage = ShaderStage::Compute) {
  ShaderReflection r;
  r.bindings.push_back({.group = 0, .slot = 0, .name = "params",
                        .kind = BindingKind::UniformBuffer,
                        .location = {.space = 0, .index = 0}});
  r.bindings.push_back({.group = 0, .slot = 1, .name = "data",
                        .kind = BindingKind::StorageBuffer,
                        .location = {.space = 0, .index = 1}});
  r.bindings.push_back({.group = 0, .slot = 2, .name = "albedo",
                        .kind = BindingKind::SampledTexture,
                        .location = {.space = 0, .index = 2}});
  r.bindings.push_back({.group = 0, .slot = 3, .name = "albedo_sampler",
                        .kind = BindingKind::Sampler,
                        .location = {.space = 0, .index = 3}});
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
  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          MakeTestReflection(), "compute_module");
  REQUIRE(module);
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main", .label = "compute"});
  REQUIRE(pipe);

  uint32_t wg[3] = {0, 0, 0};
  pipe->GetWorkgroupSize(wg);
  CHECK(wg[0] == 64);
  CHECK(wg[1] == 1);
  CHECK(wg[2] == 1);
}

inline void CheckReflectionLookupByName(IRhiDevice& device) {
  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          MakeTestReflection(), "m");
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
  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          MakeTestReflection(), "m");
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

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
  auto gfx = device.CreateShaderModule(MinimalGraphicsSource(device.GetBackend()),
                                      ShaderReflection{}, "gfx");
  auto pipe = device.CreateRenderPipeline(
      {.vertex_shader = gfx.get(), .vertex_entry = "vs_main",
       .fragment_shader = gfx.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm}, .label = "indirect_pipe"});

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
  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          MakeTestReflection(), "m");
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
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

// --- Cases that need real rasterization -------------------------------------

// DrawIndexed had NO test on either backend before this, so the
// `index_offset + first_index * stride` arithmetic in the Metal backend was
// entirely unverified. The index buffer here deliberately starts with junk and
// the draw uses first_index=3, so a wrong offset draws the junk instead.
inline void CheckIndexedDrawHonoursFirstIndex(IRhiDevice& device) {
  constexpr uint32_t kW = 16, kH = 16;
  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "verts",
                           .kind = BindingKind::ReadOnlyStorageBuffer,
                           .location = {.space = 0, .index = 0}});
  auto shader = device.CreateShaderModule(
      PullingGraphicsSource(device.GetBackend()), refl, "pull");
  REQUIRE(shader);
  auto pipe = device.CreateRenderPipeline(
      {.vertex_shader = shader.get(), .vertex_entry = "vs_main",
       .fragment_shader = shader.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "pull"});
  REQUIRE(pipe);

  // Six positions: three degenerate, then the covering triangle.
  const float verts[6][4] = {
      {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1},
      {-3, -1, 0, 1}, {3, -1, 0, 1}, {0, 3, 0, 1}};
  auto vbuf = device.CreateBuffer({.size = sizeof(verts),
                                   .usage = BufferUsage::Storage |
                                            BufferUsage::CopyDst,
                                   .label = "verts"});
  vbuf->Write(0, {reinterpret_cast<const uint8_t*>(verts), sizeof(verts)});

  const uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
  auto ibuf = device.CreateBuffer({.size = sizeof(indices),
                                   .usage = BufferUsage::Index |
                                            BufferUsage::CopyDst,
                                   .label = "indices"});
  ibuf->Write(0, {reinterpret_cast<const uint8_t*>(indices), sizeof(indices)});

  auto color = device.CreateTexture({.width = kW, .height = kH,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::RenderTarget |
                                              TextureUsage::CopySrc,
                                     .label = "indexed_target"});
  auto readback = device.CreateBuffer(
      {.size = kW * kH * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead});

  auto table = device.CreateBindingTable(
      {.render_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = vbuf.get()}}});

  auto encoder = device.CreateCommandEncoder("indexed");
  encoder->Transition(vbuf.get(), ResourceState::ShaderRead);
  encoder->Transition(color.get(), ResourceState::RenderTarget);
  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                    .load_op = LoadOp::Clear,
                                    .store_op = StoreOp::Store,
                                    .clear_color = {0, 0, 0, 1}});
  auto* pass = encoder->BeginRenderPass(desc);
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->SetIndexBuffer(ibuf.get(), IndexFormat::Uint32);
  pass->SetViewport(0, 0, float(kW), float(kH));
  pass->DrawIndexed(/*index_count=*/3, /*instance_count=*/1, /*first_index=*/3);
  pass->End();
  encoder->Transition(color.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (!ProducesPixels(device)) return;
  std::vector<uint8_t> px(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, px));
  const Rgba c = CentrePixel(px, kW, kH);
  INFO("centre = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  // Green means the draw used indices 3..5. Black means first_index was
  // ignored and the degenerate triangle was drawn instead.
  CHECK(c.g > 200);
  CHECK(c.r < 32);
}

// Proves a texture upload actually reached the GPU, which nothing did before:
// ITexture::Write was never called by any RHI test.
inline void CheckTextureUploadIsSampled(IRhiDevice& device) {
  constexpr uint32_t kW = 8, kH = 8;
  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "tex",
                           .kind = BindingKind::SampledTexture,
                           .location = {.space = 0, .index = 0}});
  refl.bindings.push_back({.group = 0, .slot = 1, .name = "samp",
                           .kind = BindingKind::Sampler,
                           .location = {.space = 0, .index = 0}});
  auto shader = device.CreateShaderModule(
      SamplingGraphicsSource(device.GetBackend()), refl, "sample");
  REQUIRE(shader);
  auto pipe = device.CreateRenderPipeline(
      {.vertex_shader = shader.get(), .vertex_entry = "vs_main",
       .fragment_shader = shader.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "sample"});
  REQUIRE(pipe);

  // A uniform, unmistakable colour, so a sample from anywhere proves upload.
  auto src = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled |
                                            TextureUsage::CopyDst,
                                   .label = "uploaded"});
  std::vector<uint8_t> texels(4 * 4 * 4);
  for (size_t i = 0; i < texels.size(); i += 4) {
    texels[i + 0] = 200; texels[i + 1] = 40; texels[i + 2] = 120; texels[i + 3] = 255;
  }
  src->Write(0, 0, AsBytes(texels));
  auto samp = device.CreateSampler({.address_u = AddressMode::ClampToEdge,
                                    .address_v = AddressMode::ClampToEdge});

  auto color = device.CreateTexture({.width = kW, .height = kH,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::RenderTarget |
                                              TextureUsage::CopySrc});
  auto readback = device.CreateBuffer(
      {.size = kW * kH * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead});

  auto table = device.CreateBindingTable(
      {.render_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::SampledTexture,
                    .texture_view = src->GetDefaultView()},
                   {.slot = 1, .kind = BindingKind::Sampler,
                    .sampler = samp.get()}}});

  auto encoder = device.CreateCommandEncoder("sample");
  encoder->Transition(src.get(), ResourceState::ShaderRead);
  encoder->Transition(color.get(), ResourceState::RenderTarget);
  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                    .load_op = LoadOp::Clear,
                                    .store_op = StoreOp::Store});
  auto* pass = encoder->BeginRenderPass(desc);
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->SetViewport(0, 0, float(kW), float(kH));
  pass->Draw(3);
  pass->End();
  encoder->Transition(color.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (!ProducesPixels(device)) return;
  std::vector<uint8_t> px(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, px));
  const Rgba c = CentrePixel(px, kW, kH);
  INFO("centre = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  CHECK(c.r == Catch::Approx(200).margin(3));
  CHECK(c.g == Catch::Approx(40).margin(3));
  CHECK(c.b == Catch::Approx(120).margin(3));
}

// LoadOp::Load must preserve what a previous pass stored. Asserted only on
// Null before this, where nothing is actually preserved or discarded.
inline void CheckLoadOpPreservesPreviousContents(IRhiDevice& device) {
  constexpr uint32_t kW = 8, kH = 8;
  auto color = device.CreateTexture({.width = kW, .height = kH,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::RenderTarget |
                                              TextureUsage::CopySrc,
                                     .label = "loadop"});
  auto readback = device.CreateBuffer(
      {.size = kW * kH * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead});

  auto encoder = device.CreateCommandEncoder("loadop");
  encoder->Transition(color.get(), ResourceState::RenderTarget);

  // Pass 1: clear to a known colour and store it.
  RenderPassDesc clear_pass;
  clear_pass.label = "clear";
  clear_pass.color_attachments.push_back(
      {.view = color->GetDefaultView(), .load_op = LoadOp::Clear,
       .store_op = StoreOp::Store, .clear_color = {1.0f, 0.5f, 0.0f, 1.0f}});
  encoder->BeginRenderPass(clear_pass)->End();

  // Pass 2: load, draw nothing, store. The colour must survive.
  RenderPassDesc load_pass;
  load_pass.label = "load";
  load_pass.color_attachments.push_back(
      {.view = color->GetDefaultView(), .load_op = LoadOp::Load,
       .store_op = StoreOp::Store});
  encoder->BeginRenderPass(load_pass)->End();

  encoder->Transition(color.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (!ProducesPixels(device)) return;
  std::vector<uint8_t> px(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, px));
  const Rgba c = CentrePixel(px, kW, kH);
  INFO("centre = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  CHECK(c.r == Catch::Approx(255).margin(3));
  CHECK(c.g == Catch::Approx(128).margin(3));
  CHECK(c.b < 8);
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
  CheckIndexedDrawHonoursFirstIndex(device);
  CheckTextureUploadIsSampled(device);
  CheckLoadOpPreservesPreviousContents(device);
}

}  // namespace badlands::rhi::test
