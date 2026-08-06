// Metal backend tests.
//
// Two halves:
//   1. The SAME conformance list badlands_rhi_tests runs against Null. That is
//      what keeps the backends from diverging as features are added.
//   2. Real GPU work with readback -- the assertions Null structurally cannot
//      make, because they depend on a shader having actually run.
//
// Shaders here are hand-written MSL rather than Slang output: the Slang layer
// is a later step, and this suite should not wait on it to prove the backend.

#include <algorithm>
#include <cmath>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "engine/rhi/metal/metal_rhi.hpp"
#include "engine/tests/rhi_conformance.hpp"

using namespace badlands::rhi;
namespace rhitest = badlands::rhi::test;

namespace {

std::unique_ptr<IRhiDevice> MakeMetal(bool validation = true) {
  return CreateDevice({.backend = BackendKind::Metal,
                       .enable_validation = validation,
                       .label = "metal_tests"});
}

// One atomic counter at buffer(0); every thread bumps it once.
constexpr const char* kBumpKernel = R"(
#include <metal_stdlib>
using namespace metal;
kernel void bump(device atomic_uint* counter [[buffer(0)]],
                 uint gid [[thread_position_in_grid]]) {
  atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}
)";

// Fullscreen triangle from the vertex id -- no vertex buffers, which is the
// MVP's vertex model (pull, don't bind).
constexpr const char* kSolidColor = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs_main(uint vid [[vertex_id]]) {
  float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
  VOut o;
  o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
fragment float4 fs_main() { return float4(0.25, 0.5, 0.75, 1.0); }
)";

ShaderReflection BumpReflection() {
  ShaderReflection r;
  r.bindings.push_back({.group = 0, .slot = 0, .name = "counter",
                        .kind = BindingKind::StorageBuffer,
                        .location = {.space = 0, .index = 0}});
  ReflectedEntryPoint ep;
  ep.name = "bump";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 32;
  r.entry_points.push_back(ep);
  return r;
}

}  // namespace

TEST_CASE("metal: device is created through the common factory", "[rhi][metal]") {
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);
  CHECK(device->GetBackend() == BackendKind::Metal);
  // No command log on a real backend -- log-guarded assertions skip themselves.
  CHECK(badlands::rhi::null::GetCommandLog(*device) == nullptr);
}

TEST_CASE("metal: the shared conformance list passes", "[rhi][metal]") {
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);
  rhitest::RunAllConformanceChecks(*device);
}

TEST_CASE("metal: the conformance list runs clean under validation",
          "[rhi][metal][validation]") {
  auto device = MakeMetal(/*validation=*/true);
  REQUIRE(device);
  device->BeginValidationScope();
  rhitest::RunAllConformanceChecks(*device);
  auto report = device->EndValidationScope();
  // A report must exist: nullopt would mean nothing was checked, which this
  // case would otherwise read as a clean run.
  REQUIRE(report.has_value());
  INFO(report->violations);
  CHECK(report->IsClean());
}

// The three cases that had no coverage at all before. They live in the shared
// list, so Null runs the same sequence; only Metal can assert the pixels.
TEST_CASE("metal: indexed draw honours first_index", "[rhi][metal][gpu]") {
  auto d = MakeMetal(false);
  REQUIRE(d);
  rhitest::CheckIndexedDrawHonoursFirstIndex(*d);
}
TEST_CASE("metal: a texture upload is visible to a shader", "[rhi][metal][gpu]") {
  auto d = MakeMetal(false);
  REQUIRE(d);
  rhitest::CheckTextureUploadIsSampled(*d);
}
TEST_CASE("metal: LoadOp::Load preserves the previous pass", "[rhi][metal][gpu]") {
  auto d = MakeMetal(false);
  REQUIRE(d);
  rhitest::CheckLoadOpPreservesPreviousContents(*d);
}

// --- Real GPU work ----------------------------------------------------------

TEST_CASE("metal: a compute dispatch with atomics produces the right count",
          "[rhi][metal][gpu]") {
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);

  auto module = device->CreateShaderModule(kBumpKernel, BumpReflection(), "bump");
  REQUIRE(module);
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "bump", .label = "bump"});
  REQUIRE(pipe);

  uint32_t wg[3] = {0, 0, 0};
  pipe->GetWorkgroupSize(wg);
  CHECK(wg[0] == 32);

  auto counter = device->CreateBuffer({.size = sizeof(uint32_t),
                                       .usage = BufferUsage::Storage |
                                                BufferUsage::MapRead |
                                                BufferUsage::CopyDst,
                                       .label = "counter"});
  REQUIRE(counter);
  const uint32_t zero = 0;
  counter->Write(0, {reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)});

  BindingTableDesc btd;
  btd.compute_pipeline = pipe.get();
  btd.label = "bump_table";
  btd.entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                  .buffer = counter.get()}};
  auto table = device->CreateBindingTable(btd);
  REQUIRE(table);

  constexpr uint32_t kGroups = 4;
  auto encoder = device->CreateCommandEncoder("compute");
  encoder->Transition(counter.get(), ResourceState::ShaderWrite);
  auto* pass = encoder->BeginComputePass("bump");
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->Dispatch(kGroups);
  pass->End();
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  uint32_t result = 0;
  REQUIRE(counter->Read(0, {reinterpret_cast<uint8_t*>(&result), sizeof(result)}));
  // Dispatch takes WORKGROUP counts, so the thread total is groups * size.
  CHECK(result == kGroups * wg[0]);
}

TEST_CASE("metal: a raster pass writes the expected pixels", "[rhi][metal][gpu]") {
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);

  auto module = device->CreateShaderModule(kSolidColor, ShaderReflection{},
                                           "solid");
  REQUIRE(module);
  auto pipe = device->CreateRenderPipeline({.vertex_shader = module.get(),
                                            .vertex_entry = "vs_main",
                                            .fragment_shader = module.get(),
                                            .fragment_entry = "fs_main",
                                            .color_formats = {Format::RGBA8Unorm},
                                            .cull_mode = CullMode::None,
                                            .label = "solid"});
  REQUIRE(pipe);

  constexpr uint32_t kW = 16, kH = 16;
  auto color = device->CreateTexture({.width = kW, .height = kH,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget |
                                               TextureUsage::CopySrc,
                                      .label = "target"});
  REQUIRE(color);
  auto readback = device->CreateBuffer({.size = kW * kH * 4,
                                        .usage = BufferUsage::CopyDst |
                                                 BufferUsage::MapRead,
                                        .label = "readback"});
  REQUIRE(readback);

  auto encoder = device->CreateCommandEncoder("raster");
  encoder->Transition(color.get(), ResourceState::RenderTarget);
  RenderPassDesc desc;
  desc.label = "solid_pass";
  desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                    .load_op = LoadOp::Clear,
                                    .store_op = StoreOp::Store,
                                    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f}});
  auto* pass = encoder->BeginRenderPass(desc);
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetViewport(0, 0, float(kW), float(kH));
  pass->Draw(3);
  pass->End();

  encoder->Transition(color.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  std::vector<uint8_t> pixels(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, pixels));

  // The fullscreen triangle covers the whole target; sample the centre.
  const size_t centre = (size_t(kH / 2) * kW + kW / 2) * 4;
  INFO("centre RGBA = " << int(pixels[centre]) << "," << int(pixels[centre + 1])
                        << "," << int(pixels[centre + 2]) << ","
                        << int(pixels[centre + 3]));
  CHECK(pixels[centre + 0] == Catch::Approx(64).margin(2));
  CHECK(pixels[centre + 1] == Catch::Approx(128).margin(2));
  CHECK(pixels[centre + 2] == Catch::Approx(191).margin(2));
  CHECK(pixels[centre + 3] == 255);
}

TEST_CASE("metal: an indirect draw honours a GPU-written count",
          "[rhi][metal][gpu]") {
  // The case Null can only half-answer: there the args are resolved from bytes
  // a test wrote, here a compute shader writes them and the draw consumes them
  // without the CPU ever reading the count back.
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);

  constexpr const char* kWriteArgs = R"(
#include <metal_stdlib>
using namespace metal;
struct Args { uint index_count; uint instance_count; uint first_index;
              int base_vertex; uint first_instance; };
kernel void write_args(device Args* args [[buffer(0)]], uint gid [[thread_position_in_grid]]) {
  if (gid != 0) return;
  args->index_count = 3;
  args->instance_count = 2;   // the value the draw must pick up
  args->first_index = 0;
  args->base_vertex = 0;
  args->first_instance = 0;
}
)";
  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "args",
                           .kind = BindingKind::StorageBuffer,
                           .location = {.space = 0, .index = 0}});
  ReflectedEntryPoint ep;
  ep.name = "write_args";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 1;
  refl.entry_points.push_back(ep);

  auto module = device->CreateShaderModule(kWriteArgs, refl, "write_args");
  REQUIRE(module);
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "write_args"});
  REQUIRE(pipe);

  auto args = device->CreateBuffer({.size = sizeof(DrawIndexedIndirectArgs),
                                    .usage = BufferUsage::Storage |
                                             BufferUsage::Indirect |
                                             BufferUsage::MapRead,
                                    .label = "args"});
  BindingTableDesc btd;
  btd.compute_pipeline = pipe.get();
  btd.entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                  .buffer = args.get()}};
  auto table = device->CreateBindingTable(btd);

  auto encoder = device->CreateCommandEncoder("gpu_driven");
  encoder->Transition(args.get(), ResourceState::ShaderWrite);
  auto* cp = encoder->BeginComputePass("write_args");
  cp->SetPipeline(pipe.get());
  cp->SetBindingTable(0, table.get());
  cp->Dispatch(1);
  cp->End();
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  DrawIndexedIndirectArgs written{};
  REQUIRE(args->Read(0, {reinterpret_cast<uint8_t*>(&written), sizeof(written)}));
  CHECK(written.index_count == 3);
  CHECK(written.instance_count == 2);
}

TEST_CASE("metal: reversed-Z depth rejects the farther fragment",
          "[rhi][metal][gpu]") {
  // Reversed-Z is a project-wide invariant, and getting the compare backwards
  // produces a plausible image rather than an error -- so it gets a real test.
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);

  // Two fullscreen triangles at different depths, red at z=0.25 then green at
  // z=0.75. Under GreaterEqual the LATER, LARGER z wins, so green survives.
  constexpr const char* kDepthShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
struct Push { float z; float r; float g; float b; };
vertex VOut vs_main(uint vid [[vertex_id]], constant Push& p [[buffer(0)]]) {
  float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
  VOut o;
  o.pos = float4(uv * 2.0 - 1.0, p.z, 1.0);
  return o;
}
fragment float4 fs_main(constant Push& p [[buffer(0)]]) {
  return float4(p.r, p.g, p.b, 1.0);
}
)";
  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "p",
                           .kind = BindingKind::UniformBuffer,
                           .location = {.space = 0, .index = 0}});

  auto module = device->CreateShaderModule(kDepthShader, refl, "depth");
  REQUIRE(module);
  auto pipe = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .vertex_entry = "vs_main",
       .fragment_shader = module.get(), .fragment_entry = "fs_main",
       .color_formats = {Format::RGBA8Unorm},
       .depth = {.test_enabled = true, .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = Format::Depth32Float},
       .cull_mode = CullMode::None, .label = "depth"});
  REQUIRE(pipe);

  constexpr uint32_t kW = 8, kH = 8;
  auto color = device->CreateTexture({.width = kW, .height = kH,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget |
                                               TextureUsage::CopySrc});
  auto depth = device->CreateTexture({.width = kW, .height = kH,
                                      .format = Format::Depth32Float,
                                      .usage = TextureUsage::DepthStencil});
  auto readback = device->CreateBuffer(
      {.size = kW * kH * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead});

  struct Push { float z, r, g, b; };
  auto near_ubo = device->CreateBuffer(
      {.size = sizeof(Push), .usage = BufferUsage::Uniform});
  auto far_ubo = device->CreateBuffer(
      {.size = sizeof(Push), .usage = BufferUsage::Uniform});
  const Push red{0.25f, 1.0f, 0.0f, 0.0f};
  const Push green{0.75f, 0.0f, 1.0f, 0.0f};
  near_ubo->Write(0, {reinterpret_cast<const uint8_t*>(&red), sizeof(red)});
  far_ubo->Write(0, {reinterpret_cast<const uint8_t*>(&green), sizeof(green)});

  auto make_table = [&](IBuffer* b) {
    BindingTableDesc d;
    d.render_pipeline = pipe.get();
    d.entries = {{.slot = 0, .kind = BindingKind::UniformBuffer, .buffer = b}};
    return device->CreateBindingTable(d);
  };
  auto t_red = make_table(near_ubo.get());
  auto t_green = make_table(far_ubo.get());

  auto encoder = device->CreateCommandEncoder("depth");
  encoder->Transition(color.get(), ResourceState::RenderTarget);
  encoder->Transition(depth.get(), ResourceState::DepthWrite);

  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView(),
                                    .load_op = LoadOp::Clear,
                                    .store_op = StoreOp::Store});
  // Reversed-Z clears to 0.0 = far.
  desc.depth_attachment = {.view = depth->GetDefaultView(),
                           .load_op = LoadOp::Clear,
                           .store_op = StoreOp::Store,
                           .clear_depth = 0.0f};
  auto* pass = encoder->BeginRenderPass(desc);
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetViewport(0, 0, float(kW), float(kH));
  pass->SetBindingTable(0, t_red.get());
  pass->Draw(3);
  pass->SetBindingTable(0, t_green.get());
  pass->Draw(3);
  pass->End();

  encoder->Transition(color.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(color.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  std::vector<uint8_t> pixels(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, pixels));
  const size_t centre = (size_t(kH / 2) * kW + kW / 2) * 4;
  INFO("centre RGBA = " << int(pixels[centre]) << "," << int(pixels[centre + 1])
                        << "," << int(pixels[centre + 2]));
  // Green (z=0.75) beat red (z=0.25) because GreaterEqual keeps the larger z.
  CHECK(pixels[centre + 0] < 32);
  CHECK(pixels[centre + 1] > 200);
}

// Creation-time refusals, the same list the Null suite runs. Metal used to be
// the only backend that refused a destroyed texture, and only a shared list
// makes that kind of divergence visible (rule 6).
TEST_CASE("metal: an unretainable entry is refused at creation", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckUnretainableEntryIsRefused(*d);
}
TEST_CASE("metal: a view with no owning texture is refused", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckOwnerlessViewIsRefused(*d);
}
TEST_CASE("metal: an unresolvable slot is refused at creation", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckUnresolvableSlotIsRefused(*d);
}
TEST_CASE("metal: a mismatched binding kind is refused at creation",
          "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckMismatchedKindIsRefused(*d);
}
TEST_CASE("metal: a table with no pipeline is refused", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckTableWithNoPipelineIsRefused(*d);
}
TEST_CASE("metal: a resolvable table is created silently", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckResolvableTableIsCreatedSilently(*d);
}
TEST_CASE("metal: CreateView after Destroy is refused", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckCreateViewAfterDestroyIsRefused(*d);
}

TEST_CASE("metal: frames advance and pace", "[rhi][metal][gpu]") {
  auto d = MakeMetal();
  rhitest::CheckFramesAdvanceAndPace(*d);
}
TEST_CASE("metal: skipped frames still retire", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckSkippedFramesStillRetire(*d);
}

TEST_CASE("metal: Destroy is deferred to frame retirement", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDestroyIsDeferredToFrameRetirement(*d);
}
TEST_CASE("metal: Destroy outside a frame is immediate", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDestroyOutsideAFrameIsImmediate(*d);
}
TEST_CASE("metal: resource ids are never reused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckResourceIdsAreUnique(*d);
}

TEST_CASE("metal: frame allocator basics", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFrameAllocatorBasics(*d);
}
TEST_CASE("metal: frame allocator refusals", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFrameAllocatorRefusals(*d);
}
TEST_CASE("metal: frame allocator grows then caps", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFrameAllocatorGrowsThenCaps(*d);
}
TEST_CASE("metal: frame allocator recycles per slot", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFrameAllocatorRecyclesPerSlot(*d);
}

TEST_CASE("metal: frame allocator survives growth failure", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFrameAllocatorSurvivesGrowthFailure(*d);
}
TEST_CASE("metal: wild buffer offsets are refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckWildBufferOffsetsAreRefused(*d);
}

TEST_CASE("metal: an unaligned base offset is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckUnalignedBaseOffsetIsRefused(*d);
}
TEST_CASE("metal: a base offset past the buffer is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckBaseOffsetPastTheBufferIsRefused(*d);
}
TEST_CASE("metal: an unimplemented buffer_size is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckUnimplementedBufferSizeIsRefused(*d);
}
TEST_CASE("metal: WaitIdle does not retire the open frame", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckWaitIdleDoesNotRetireTheOpenFrame(*d);
}

TEST_CASE("metal: dynamic offsets reach the backend", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDynamicOffsetsReachTheBackend(*d);
}

TEST_CASE("metal: indirect dispatch reads its count", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDispatchIndirectReadsItsCount(*d);
}
TEST_CASE("metal: a zero indirect dispatch is allowed", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckZeroIndirectDispatchIsAllowed(*d);
}
TEST_CASE("metal: an indirect dispatch with no args is refused", "[rhi]") {
  // Unvalidated: the decorator refuses this first, so with validation on the
  // backend never sees it.
  auto d = MakeMetal(/*validation=*/false);
  rhitest::CheckIndirectDispatchWithoutArgsIsRefused(*d);
}
TEST_CASE("metal: a draw with no pipeline is refused", "[rhi]") {
  auto d = MakeMetal(/*validation=*/false);
  rhitest::CheckDrawWithoutPipelineIsRefused(*d);
}
TEST_CASE("metal: the feature query answers", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckFeatureQueryAnswers(*d);
}
TEST_CASE("metal: a mismatched blend state count is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckMismatchedBlendStateCountIsRefused(*d);
}
TEST_CASE("metal: an opaque pipeline needs no blend state", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckOpaquePipelineNeedsNoBlendState(*d);
}

TEST_CASE("metal: an indirect dispatch really runs the count in the buffer",
          "[rhi][metal]") {
  // The shared case can only assert through the Null command log, so on Metal
  // it executes NO assertion at all -- an implementation that passed the wrong
  // offset, used the wrong threadgroup size, or did nothing would still pass
  // it. The only real Metal proof lived in badlands_splat_tests, which is
  // skipped entirely without a Slang SDK.
  auto d = MakeMetal();
  REQUIRE(d);

  auto module = d->CreateShaderModule(kBumpKernel, BumpReflection(), "bump");
  REQUIRE(module);
  auto pipe = d->CreateComputePipeline(
      {.shader = module.get(), .entry = "bump", .label = "bump"});
  REQUIRE(pipe);

  auto counter = d->CreateBuffer(
      {.size = sizeof(uint32_t),
       .usage = BufferUsage::Storage | BufferUsage::CopyDst |
                BufferUsage::MapRead, .label = "counter"});
  auto args = d->CreateBuffer(
      {.size = sizeof(DispatchIndirectArgs),
       .usage = BufferUsage::Indirect | BufferUsage::CopyDst, .label = "args"});
  REQUIRE(counter);
  REQUIRE(args);

  const uint32_t zero = 0;
  counter->Write(0, {reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)});
  // Three groups of the kernel's declared 64 threads.
  const DispatchIndirectArgs seeded{.x = 3, .y = 1, .z = 1};
  args->Write(0, {reinterpret_cast<const uint8_t*>(&seeded), sizeof(seeded)});

  auto table = d->CreateBindingTable(
      {.compute_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                    .buffer = counter.get()}},
       .label = "bump_table"});
  REQUIRE(table);

  auto encoder = d->CreateCommandEncoder("indirect_bump");
  encoder->Transition(counter.get(), ResourceState::ShaderWrite);
  encoder->Transition(args.get(), ResourceState::IndirectArg);
  auto* pass = encoder->BeginComputePass("cs");
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->DispatchIndirect(args.get(), 0);
  pass->End();
  encoder->Finish();
  d->Submit(*encoder);
  d->WaitIdle();

  uint32_t got = 0;
  REQUIRE(counter->Read(0, {reinterpret_cast<uint8_t*>(&got), sizeof(got)}));
  uint32_t threads[3] = {1, 1, 1};
  pipe->GetWorkgroupSize(threads);
  INFO("got=" << got << " threads/group=" << threads[0]);
  CHECK(got == seeded.x * threads[0]);
}

TEST_CASE("metal: swapchain acquire/present cycle", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckSwapchainAcquirePresentCycle(*d);
}
TEST_CASE("metal: swapchain skips when zero-sized", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckSwapchainSkipsWhenZeroSized(*d);
}
TEST_CASE("metal: swapchain resize", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckSwapchainResize(*d);
}
TEST_CASE("metal: too many dynamic offsets are refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckTooManyDynamicOffsetsAreRefused(*d);
}
TEST_CASE("metal: a dynamic offset on a non-buffer is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDynamicOffsetOnANonBufferIsRefused(*d);
}

// --- The primitive the splat technique rests on ----------------------------

TEST_CASE("metal: 64-bit atomic max carries its payload indivisibly",
          "[rhi][metal][gpu]") {
  // Pack depth above payload and ONE atomic resolves the depth test and
  // commits the payload together. That indivisibility is the whole point: with
  // a 32-bit depth atomic plus a separate payload write, thread A can win the
  // depth test and thread B can write the payload between A's two stores.
  //
  // This is hand-written MSL, not Slang, because Slang 2026.14.1 emits
  // atomic_fetch_max_explicit (the 32-bit family) for Metal, which MSL rejects
  // for 64-bit types. The correct spelling is atomic_max_explicit on a
  // device atomic_ulong*, which returns void and is device-address-space only.
  constexpr const char* kAtomicMax = R"(
#include <metal_stdlib>
using namespace metal;
kernel void cs_main(device atomic_ulong* slot [[buffer(0)]],
                    device const uint* depths [[buffer(1)]],
                    uint tid [[thread_position_in_grid]]) {
  // Payload is the thread index, so the winner is identifiable.
  ulong packed = (ulong(depths[tid]) << 32) | ulong(tid);
  atomic_max_explicit(&slot[0], packed, memory_order_relaxed);
}
)";
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);
  // REQUIRED, not skipped: a Metal device below the recorded M2 floor is a
  // configuration error, and skipping would leave a silent hole exactly where
  // the port's founding claim is tested.
  REQUIRE(device->Supports(DeviceFeature::Atomic64MinMax));

  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "slot",
                           .kind = BindingKind::StorageBuffer,
                           .location = {.space = 0, .index = 0}});
  refl.bindings.push_back({.group = 0, .slot = 1, .name = "depths",
                           .kind = BindingKind::ReadOnlyStorageBuffer,
                           .location = {.space = 0, .index = 1}});
  ReflectedEntryPoint ep;
  ep.name = "cs_main";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 64;
  refl.entry_points.push_back(ep);

  auto module = device->CreateShaderModule(kAtomicMax, refl, "atomic64");
  REQUIRE(module);
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  REQUIRE(pipe);

  // A hash, so the maximum is NOT the last thread -- a kernel that simply let
  // the final write win would pass against a monotonic sequence.
  constexpr uint32_t kThreads = 64 * 16;
  std::vector<uint32_t> depths(kThreads);
  uint32_t best_depth = 0, best_tid = 0;
  for (uint32_t i = 0; i < kThreads; ++i) {
    depths[i] = uint32_t(i * 2654435761u) >> 8;  // >>8 keeps ties impossible
    if (depths[i] > best_depth) { best_depth = depths[i]; best_tid = i; }
  }

  auto slot = device->CreateBuffer({.size = sizeof(uint64_t),
                                    .usage = BufferUsage::Storage |
                                             BufferUsage::MapRead |
                                             BufferUsage::CopyDst,
                                    .label = "slot"});
  auto depth_buf = device->CreateBuffer(
      {.size = kThreads * sizeof(uint32_t),
       .usage = BufferUsage::Storage | BufferUsage::CopyDst,
       .label = "depths"});
  REQUIRE(slot);
  REQUIRE(depth_buf);
  const uint64_t zero = 0;
  slot->Write(0, {reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)});
  depth_buf->Write(0, {reinterpret_cast<const uint8_t*>(depths.data()),
                       depths.size() * sizeof(uint32_t)});

  auto table = device->CreateBindingTable(
      {.compute_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                    .buffer = slot.get()},
                   {.slot = 1, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = depth_buf.get()}},
       .label = "atomic64"});
  REQUIRE(table);

  auto encoder = device->CreateCommandEncoder("atomic64");
  encoder->Transition(slot.get(), ResourceState::ShaderWrite);
  encoder->Transition(depth_buf.get(), ResourceState::ShaderRead);
  auto* pass = encoder->BeginComputePass("cs");
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->Dispatch(kThreads / 64);
  pass->End();
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  uint64_t result = 0;
  REQUIRE(slot->Read(0, {reinterpret_cast<uint8_t*>(&result), sizeof(result)}));
  const uint32_t got_depth = uint32_t(result >> 32);
  const uint32_t got_tid = uint32_t(result & 0xFFFFFFFFu);
  INFO("packed=" << result << " depth=" << got_depth << " tid=" << got_tid
                 << " expected depth=" << best_depth << " tid=" << best_tid);
  CHECK(got_depth == best_depth);
  // The payload must be the WINNER'S, not some other thread's. This is the
  // half a 32-bit depth atomic cannot guarantee.
  CHECK(got_tid == best_tid);
}

TEST_CASE("metal: a dynamic offset changes what the shader reads",
          "[rhi][metal][gpu]") {
  // The conformance case only inspects Null's command log, which does not
  // exist on Metal -- so on this backend it asserts nothing about the offset
  // actually being applied. Its red proof caught that. This reads the value
  // back off the GPU: same table, two offsets, two different results.
  constexpr const char* kReadAtOffset = R"(
#include <metal_stdlib>
using namespace metal;
kernel void cs_main(constant uint* src [[buffer(0)]],
                    device uint* dst [[buffer(1)]],
                    uint gid [[thread_position_in_grid]]) {
  dst[0] = src[0];
}
)";
  ShaderReflection refl;
  refl.bindings.push_back({.group = 0, .slot = 0, .name = "src",
                           .kind = BindingKind::UniformBuffer,
                           .location = {.space = 0, .index = 0}});
  refl.bindings.push_back({.group = 0, .slot = 1, .name = "dst",
                           .kind = BindingKind::StorageBuffer,
                           .location = {.space = 0, .index = 1}});
  ReflectedEntryPoint ep;
  ep.name = "cs_main";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 1;
  refl.entry_points.push_back(ep);

  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);
  auto module = device->CreateShaderModule(kReadAtOffset, refl, "readoff");
  auto pipe = device->CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  REQUIRE(pipe);

  const uint64_t align = device->MinBufferOffsetAlignment();
  auto src = device->CreateBuffer({.size = align * 4,
                                   .usage = BufferUsage::Uniform,
                                   .label = "src"});
  auto dst = device->CreateBuffer({.size = sizeof(uint32_t),
                                   .usage = BufferUsage::Storage |
                                            BufferUsage::MapRead,
                                   .label = "dst"});
  REQUIRE(src);
  REQUIRE(dst);

  // Distinct values one alignment apart, so the offset is the only thing that
  // can select between them.
  const uint32_t kFirst = 0xAAAA1111u, kSecond = 0xBBBB2222u;
  src->Write(0, {reinterpret_cast<const uint8_t*>(&kFirst), sizeof(kFirst)});
  src->Write(align,
             {reinterpret_cast<const uint8_t*>(&kSecond), sizeof(kSecond)});

  auto table = device->CreateBindingTable(
      {.compute_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = src.get(), .dynamic_offset = true},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = dst.get()}},
       .label = "dyn_read"});
  REQUIRE(table);

  auto run_with = [&](uint32_t offset) {
    const uint32_t offsets[1] = {offset};
    auto encoder = device->CreateCommandEncoder("dyn");
    encoder->Transition(src.get(), ResourceState::ShaderRead);
    encoder->Transition(dst.get(), ResourceState::ShaderWrite);
    auto* pass = encoder->BeginComputePass("dyn");
    pass->SetPipeline(pipe.get());
    pass->SetBindingTable(0, table.get(), offsets);
    pass->Dispatch(1);
    pass->End();
    encoder->Finish();
    device->Submit(*encoder);
    device->WaitIdle();
    uint32_t out = 0;
    REQUIRE(dst->Read(0, {reinterpret_cast<uint8_t*>(&out), sizeof(out)}));
    return out;
  };

  CHECK(run_with(0) == kFirst);
  CHECK(run_with(uint32_t(align)) == kSecond);
}

TEST_CASE("metal: a short dynamic-offset span is refused", "[rhi][metal]") {
  // The decorator catches this too, but it compiles out of a release build.
  // Binding at base offsets instead would leave every dynamic binding pointing
  // at frame 0's data forever, with nothing logged anywhere.
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  auto pipe = rhitest::MakeTestPipeline(*d);
  REQUIRE(pipe);
  auto ubo = d->CreateBuffer({.size = 1024, .usage = BufferUsage::Uniform});
  auto ssbo = d->CreateBuffer({.size = 1024, .usage = BufferUsage::Storage});
  auto tex = d->CreateTexture({.width = 4, .height = 4,
                               .format = Format::RGBA8Unorm,
                               .usage = TextureUsage::Sampled});
  auto samp = d->CreateSampler({});
  auto table = d->CreateBindingTable(
      {.compute_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = ubo.get(), .dynamic_offset = true},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = ssbo.get()},
                   {.slot = 2, .kind = BindingKind::SampledTexture,
                    .texture_view = tex->GetDefaultView()},
                   {.slot = 3, .kind = BindingKind::Sampler,
                    .sampler = samp.get()}},
       .label = "shortspan"});
  REQUIRE(table);

  const std::string log = rhitest::CaptureLog([&] {
    auto encoder = d->CreateCommandEncoder("short");
    auto* pass = encoder->BeginComputePass("cp");
    pass->SetPipeline(pipe.get());
    pass->SetBindingTable(0, table.get());  // declares one, supplies none
    pass->End();
    encoder->Finish();
    d->Submit(*encoder);
    d->WaitIdle();
  });
  INFO(log);
  CHECK(log.find("binding nothing rather than guessing") != std::string::npos);
}

TEST_CASE("metal: a deferred handle is really released rather than stranded",
          "[rhi][metal]") {
  // ASan cannot see this. It catches memory freed and then touched, but an
  // Objective-C object whose retain count never reaches zero is not freed at
  // all -- it just leaks, and every test still passes. A __weak reference goes
  // nil exactly when the last strong reference goes away, so it is the only
  // automated way to assert that deferring a handle does not also strand it.
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  CHECK(badlands::rhi::metal::WeakHandleClearedAfterRetire(*d));
}
TEST_CASE("metal: destroying a device mid-frame is diagnosed not fatal",
          "[rhi][metal]") {
  // BeginFrame takes a semaphore count that only EndFrame returns. libdispatch
  // TRAPS on destroying a semaphore below its initial value, so an error path
  // that returned between the two crashed inside libdispatch with none of the
  // caller's frames in the backtrace -- pointing at the wrong thing entirely.
  const std::string log = rhitest::CaptureLog([&] {
    auto d = MakeMetal(/*validation=*/false);
    REQUIRE(d);
    d->BeginFrame();
    // Destroyed here, with the frame still open.
  });
  INFO(log);
  CHECK(log.find("still open") != std::string::npos);
}

TEST_CASE("metal: sliced views honour their range", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckSlicedViewsHonourTheirRange(*d);
}
TEST_CASE("metal: out-of-range views are refused", "[rhi][metal]") {
  auto d = MakeMetal();
  rhitest::CheckOutOfRangeViewsAreRefused(*d);
}
TEST_CASE("metal: views survive Destroy on their texture", "[rhi][metal]") {
  auto d = MakeMetal(false);
  REQUIRE(d);
  rhitest::CheckViewsSurviveTextureDestroy(*d);
}
TEST_CASE("metal: a binding table retains its resources", "[rhi][metal]") {
  auto d = MakeMetal(false);
  REQUIRE(d);
  rhitest::CheckBindingTableRetainsItsResources(*d);
}

TEST_CASE("metal: submissions retire and stop accumulating", "[rhi][metal][gpu]") {
  // Before this, Submit appended every command buffer and only WaitIdle ever
  // cleared the list, so a long-running app retained every submission it had
  // ever made. This also pins the retirement signal a frame model will use.
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);
  CHECK(device->InFlightCount() == 0);

  for (int i = 0; i < 8; ++i) {
    auto encoder = device->CreateCommandEncoder("retire");
    encoder->Finish();
    device->Submit(*encoder);
  }
  device->WaitIdle();
  CHECK(device->InFlightCount() == 0);

  // Submitting again after everything retired must not resurrect the old ones.
  auto encoder = device->CreateCommandEncoder("retire_again");
  encoder->Finish();
  device->Submit(*encoder);
  CHECK(device->InFlightCount() <= 1);
  device->WaitIdle();
  CHECK(device->InFlightCount() == 0);
}

TEST_CASE("metal: a submission keeps its resources alive after the caller drops them",
          "[rhi][metal][gpu]") {
  // GPU-timeline lifetime. Metal provides this natively by retaining command
  // buffer references, so this test cannot FAIL on Metal -- it exists to pin
  // the contract that a DX12 backend has to implement itself, and to be the
  // test that fails there if it does not.
  auto device = MakeMetal(/*validation=*/false);
  REQUIRE(device);

  auto readback = device->CreateBuffer(
      {.size = 64, .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "survivor"});
  auto encoder = device->CreateCommandEncoder("inflight");
  {
    auto src = device->CreateBuffer(
        {.size = 64, .usage = BufferUsage::CopySrc | BufferUsage::CopyDst,
         .label = "dropped"});
    std::vector<uint8_t> bytes(64, 0xA5);
    src->Write(0, {bytes.data(), bytes.size()});
    encoder->CopyBufferToBuffer(src.get(), 0, readback.get(), 0, 64);
    encoder->Finish();
    device->Submit(*encoder);
    // `src` dies here, while the GPU may still be copying from it.
  }
  device->WaitIdle();

  std::vector<uint8_t> out(64, 0);
  REQUIRE(readback->Read(0, out));
  CHECK(out[0] == 0xA5);
  CHECK(out[63] == 0xA5);
}

// --- Blending ---------------------------------------------------------------
//
// Blend state is the one piece of pipeline state whose correctness is invisible
// to every other test in this suite: a backend that swapped src and dst, or
// wired alpha from the colour component, still renders a plausible image. So
// every enumerator the RHI exposes is compared against a CPU evaluation of the
// blend equation -- the oracle pattern that pinned the splat count.
//
// Rule 4 is why this is exhaustive rather than representative: a factor that is
// accepted and mismapped is exactly the "advertised but unimplemented" trap,
// and the only way the enum stays honest is that nothing in it is untested.

namespace {

// Chosen so every channel is an exact multiple of 1/255 (51, 102, 153, 204),
// so quantisation is not a source of disagreement -- and so no channel is 0 or
// 1, where a swapped factor would coincidentally produce the right answer.
// Alphas differ from each other AND from 1, which is what makes DstAlpha and
// the separate-alpha case distinguishable at all.
constexpr glm::vec4 kSrc{0.8f, 0.6f, 0.4f, 0.6f};
constexpr glm::vec4 kDst{0.2f, 0.4f, 0.6f, 0.8f};

// A constant colour from a uniform, over a fullscreen triangle.
constexpr const char* kConstantColor = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs_main(uint vid [[vertex_id]]) {
  float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
  VOut o;
  o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
fragment float4 fs_main(constant float4& color [[buffer(0)]]) { return color; }
)";

ShaderReflection ConstantColorReflection() {
  ShaderReflection r;
  r.bindings.push_back({.group = 0, .slot = 0, .name = "color",
                        .kind = BindingKind::UniformBuffer,
                        .location = {.space = 0, .index = 0}});
  return r;
}

// The value a factor multiplies its operand by. Per-channel for the colour
// component; the alpha component uses only .a.
glm::vec4 FactorValue(BlendFactor f, glm::vec4 s, glm::vec4 d) {
  switch (f) {
    case BlendFactor::Zero: return glm::vec4(0.0f);
    case BlendFactor::One: return glm::vec4(1.0f);
    case BlendFactor::Src: return s;
    case BlendFactor::OneMinusSrc: return glm::vec4(1.0f) - s;
    case BlendFactor::SrcAlpha: return glm::vec4(s.a);
    case BlendFactor::OneMinusSrcAlpha: return glm::vec4(1.0f - s.a);
    // RGB saturates against the destination's remaining alpha; the ALPHA
    // channel of this factor is defined as 1, not as the saturated value.
    case BlendFactor::SrcAlphaSaturated: {
      const float k = std::min(s.a, 1.0f - d.a);
      return glm::vec4(k, k, k, 1.0f);
    }
    case BlendFactor::Dst: return d;
    case BlendFactor::OneMinusDst: return glm::vec4(1.0f) - d;
    case BlendFactor::DstAlpha: return glm::vec4(d.a);
    case BlendFactor::OneMinusDstAlpha: return glm::vec4(1.0f - d.a);
  }
  return glm::vec4(1.0f);
}

// `a` and `b` are already factored, EXCEPT for Min/Max -- see below.
float Combine(BlendOp op, float factored_s, float factored_d, float raw_s,
              float raw_d) {
  switch (op) {
    case BlendOp::Add: return factored_s + factored_d;
    case BlendOp::Subtract: return factored_s - factored_d;
    case BlendOp::ReverseSubtract: return factored_d - factored_s;
    // Min and Max IGNORE the factors entirely, on Metal and on D3D12 alike.
    // Modelling that here rather than in a comment is the only way the test
    // can tell a backend that honours it from one that does not.
    case BlendOp::Min: return std::min(raw_s, raw_d);
    case BlendOp::Max: return std::max(raw_s, raw_d);
  }
  return factored_s + factored_d;
}

glm::vec4 CpuBlend(const BlendState& b, glm::vec4 s, glm::vec4 d) {
  if (!b.enabled) return s;
  const glm::vec4 cs = FactorValue(b.color.src, s, d);
  const glm::vec4 cd = FactorValue(b.color.dst, s, d);
  const glm::vec4 as = FactorValue(b.alpha.src, s, d);
  const glm::vec4 ad = FactorValue(b.alpha.dst, s, d);
  glm::vec4 out;
  for (int i = 0; i < 3; ++i) {
    out[i] = Combine(b.color.op, s[i] * cs[i], d[i] * cd[i], s[i], d[i]);
  }
  out.a = Combine(b.alpha.op, s.a * as.a, d.a * ad.a, s.a, d.a);
  return glm::clamp(out, 0.0f, 1.0f);  // Unorm target
}

struct Texel { uint8_t r, g, b, a; };

// Draws kDst opaque, then kSrc through `blend`, into an RGBA8Unorm target.
// `use_blend_states` false omits the vector entirely, which is the path every
// existing pipeline in the engine takes.
Texel RenderBlended(IRhiDevice& dev, const BlendState& blend,
                    bool use_blend_states = true) {
  constexpr uint32_t kW = 4, kH = 4;
  auto module = dev.CreateShaderModule(kConstantColor,
                                       ConstantColorReflection(), "const_color");
  REQUIRE(module);

  RenderPipelineDesc base{.vertex_shader = module.get(),
                          .vertex_entry = "vs_main",
                          .fragment_shader = module.get(),
                          .fragment_entry = "fs_main",
                          .color_formats = {Format::RGBA8Unorm},
                          .cull_mode = CullMode::None};
  RenderPipelineDesc opaque_desc = base;
  opaque_desc.label = "blend_dst";
  RenderPipelineDesc blend_desc = base;
  blend_desc.label = "blend_src";
  if (use_blend_states) blend_desc.blend_states = {blend};

  auto opaque_pipe = dev.CreateRenderPipeline(opaque_desc);
  auto blend_pipe = dev.CreateRenderPipeline(blend_desc);
  REQUIRE(opaque_pipe);
  REQUIRE(blend_pipe);

  auto make_color_buf = [&](glm::vec4 c, const char* label) {
    auto b = dev.CreateBuffer({.size = sizeof(glm::vec4),
                               .usage = BufferUsage::Uniform |
                                        BufferUsage::CopyDst,
                               .label = label});
    REQUIRE(b);
    b->Write(0, {reinterpret_cast<const uint8_t*>(&c), sizeof(c)});
    return b;
  };
  auto dst_buf = make_color_buf(kDst, "dst_color");
  auto src_buf = make_color_buf(kSrc, "src_color");

  auto make_table = [&](IRenderPipeline* p, IBuffer* buf, const char* label) {
    auto t = dev.CreateBindingTable(
        {.render_pipeline = p,
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = buf}},
         .label = label});
    REQUIRE(t);
    return t;
  };
  auto dst_table = make_table(opaque_pipe.get(), dst_buf.get(), "dst_table");
  auto src_table = make_table(blend_pipe.get(), src_buf.get(), "src_table");

  auto target = dev.CreateTexture({.width = kW, .height = kH,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::RenderTarget |
                                            TextureUsage::CopySrc,
                                   .label = "blend_target"});
  auto readback = dev.CreateBuffer({.size = kW * kH * 4,
                                    .usage = BufferUsage::CopyDst |
                                             BufferUsage::MapRead,
                                    .label = "blend_readback"});
  REQUIRE(target);
  REQUIRE(readback);

  auto encoder = dev.CreateCommandEncoder("blend");
  encoder->Transition(dst_buf.get(), ResourceState::ShaderRead);
  encoder->Transition(src_buf.get(), ResourceState::ShaderRead);
  encoder->Transition(target.get(), ResourceState::RenderTarget);
  RenderPassDesc rp;
  rp.label = "blend";
  rp.color_attachments.push_back({.view = target->GetDefaultView(),
                                  .load_op = LoadOp::Clear,
                                  .store_op = StoreOp::Store,
                                  .clear_color = {0, 0, 0, 0}});
  auto* pass = encoder->BeginRenderPass(rp);
  REQUIRE(pass != nullptr);
  pass->SetViewport(0, 0, float(kW), float(kH));
  // Destination first, unblended, so what follows blends against a known
  // framebuffer rather than against a clear colour chosen for convenience.
  pass->SetPipeline(opaque_pipe.get());
  pass->SetBindingTable(0, dst_table.get());
  pass->Draw(3);
  pass->SetPipeline(blend_pipe.get());
  pass->SetBindingTable(0, src_table.get());
  pass->Draw(3);
  pass->End();

  encoder->Transition(target.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(target.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  dev.Submit(*encoder);
  dev.WaitIdle();

  std::vector<uint8_t> px(kW * kH * 4, 0);
  REQUIRE(readback->Read(0, px));
  return {px[0], px[1], px[2], px[3]};
}

Texel Quantise(glm::vec4 c) {
  auto q = [](float v) {
    return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return {q(c.r), q(c.g), q(c.b), q(c.a)};
}

// +/- 1 LSB: the GPU blends in float and rounds once, and reproducing its exact
// rounding mode is not what this test is about.
void CheckTexel(Texel got, Texel want, const std::string& what) {
  INFO(what << ": got (" << int(got.r) << "," << int(got.g) << ","
            << int(got.b) << "," << int(got.a) << ") want (" << int(want.r)
            << "," << int(want.g) << "," << int(want.b) << "," << int(want.a)
            << ")");
  CHECK(std::abs(int(got.r) - int(want.r)) <= 1);
  CHECK(std::abs(int(got.g) - int(want.g)) <= 1);
  CHECK(std::abs(int(got.b) - int(want.b)) <= 1);
  CHECK(std::abs(int(got.a) - int(want.a)) <= 1);
}

constexpr BlendFactor kAllFactors[] = {
    BlendFactor::Zero,        BlendFactor::One,
    BlendFactor::Src,         BlendFactor::OneMinusSrc,
    BlendFactor::SrcAlpha,    BlendFactor::OneMinusSrcAlpha,
    BlendFactor::SrcAlphaSaturated,
    BlendFactor::Dst,         BlendFactor::OneMinusDst,
    BlendFactor::DstAlpha,    BlendFactor::OneMinusDstAlpha,
};

constexpr BlendOp kAllOps[] = {BlendOp::Add, BlendOp::Subtract,
                               BlendOp::ReverseSubtract, BlendOp::Min,
                               BlendOp::Max};

}  // namespace

TEST_CASE("metal: every blend factor matches the CPU blend equation",
          "[rhi][metal][gpu]") {
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  for (BlendFactor f : kAllFactors) {
    // Once on the source side, once on the destination side. A backend that
    // maps a factor correctly in one direction and not the other is a real
    // failure mode, and testing only one side cannot see it.
    for (bool on_src : {true, false}) {
      BlendState b{.enabled = true};
      b.color = {.src = on_src ? f : BlendFactor::One,
                 .dst = on_src ? BlendFactor::One : f,
                 .op = BlendOp::Add};
      b.alpha = b.color;
      CheckTexel(RenderBlended(*d, b), Quantise(CpuBlend(b, kSrc, kDst)),
                 std::string(ToString(f)) + (on_src ? " as src" : " as dst"));
    }
  }
}

TEST_CASE("metal: every blend op matches the CPU blend equation",
          "[rhi][metal][gpu]") {
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  for (BlendOp op : kAllOps) {
    BlendState b{.enabled = true};
    b.color = {.src = BlendFactor::SrcAlpha,
               .dst = BlendFactor::OneMinusSrcAlpha, .op = op};
    b.alpha = b.color;
    CheckTexel(RenderBlended(*d, b), Quantise(CpuBlend(b, kSrc, kDst)),
               ToString(op));
  }
}

TEST_CASE("metal: colour and alpha blend independently", "[rhi][metal][gpu]") {
  // rgb takes the DESTINATION, alpha takes the SOURCE. A backend that wires the
  // alpha component from the colour component produces kDst.a here instead of
  // kSrc.a -- and the two differ, which is why the constants were chosen that
  // way.
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  BlendState b{.enabled = true};
  b.color = {.src = BlendFactor::Zero, .dst = BlendFactor::One,
             .op = BlendOp::Add};
  b.alpha = {.src = BlendFactor::One, .dst = BlendFactor::Zero,
             .op = BlendOp::Add};

  const Texel got = RenderBlended(*d, b);
  CheckTexel(got, Quantise(glm::vec4(kDst.r, kDst.g, kDst.b, kSrc.a)),
             "separate components");
  // Spelled out, so the intent survives a change to the constants.
  CHECK(std::abs(int(got.a) - int(Quantise(kSrc).a)) <= 1);
  CHECK(std::abs(int(got.a) - int(Quantise(kDst).a)) > 1);
}

TEST_CASE("metal: Min and Max ignore their factors", "[rhi][metal][gpu]") {
  // Factors of Zero on both sides. If they were applied, the result would be
  // min/max(0, 0) == 0; because they are ignored, it is min/max(src, dst).
  // Neither is 0, so the two answers cannot be confused.
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  for (BlendOp op : {BlendOp::Min, BlendOp::Max}) {
    BlendState b{.enabled = true};
    b.color = {.src = BlendFactor::Zero, .dst = BlendFactor::Zero, .op = op};
    b.alpha = b.color;

    const Texel got = RenderBlended(*d, b);
    CheckTexel(got, Quantise(CpuBlend(b, kSrc, kDst)), ToString(op));
    CHECK(got.r > 0);  // the factors-were-applied answer
  }
}

TEST_CASE("metal: a disabled blend state is bit-identical to none at all",
          "[rhi][metal][gpu]") {
  // The regression that matters most: every pipeline in the engine takes the
  // no-blend-state path, so if `enabled = false` diverged from it the damage
  // would be everywhere and silent.
  auto d = MakeMetal(/*validation=*/false);
  REQUIRE(d);
  // The factors are those of real alpha blending, and only `enabled` says not
  // to use them. Leaving them at BlendState{}'s defaults would make this case
  // VACUOUS: src=One, dst=Zero, op=Add is the pass-through equation, so a
  // backend that ignored `enabled` entirely would produce identical pixels and
  // this test would pass while proving nothing. (It did, until a red proof
  // failed to go red.)
  BlendState disabled = AlphaBlend();
  disabled.enabled = false;

  const Texel none = RenderBlended(*d, BlendState{}, /*use_blend_states=*/false);
  const Texel disabled_px = RenderBlended(*d, disabled);

  INFO("none=(" << int(none.r) << "," << int(none.g) << "," << int(none.b)
                << "," << int(none.a) << ") disabled=(" << int(disabled_px.r)
                << "," << int(disabled_px.g) << "," << int(disabled_px.b) << ","
                << int(disabled_px.a) << ")");
  CHECK(none.r == disabled_px.r);
  CHECK(none.g == disabled_px.g);
  CHECK(none.b == disabled_px.b);
  CHECK(none.a == disabled_px.a);
  // ...and both are the source, untouched by the destination underneath.
  CheckTexel(none, Quantise(kSrc), "opaque overwrite");
}
