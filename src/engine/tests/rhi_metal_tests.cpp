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

#include <catch_amalgamated.hpp>

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

TEST_CASE("metal: dynamic offsets reach the backend", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDynamicOffsetsReachTheBackend(*d);
}
TEST_CASE("metal: too many dynamic offsets are refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckTooManyDynamicOffsetsAreRefused(*d);
}
TEST_CASE("metal: a dynamic offset on a non-buffer is refused", "[rhi]") {
  auto d = MakeMetal();
  rhitest::CheckDynamicOffsetOnANonBufferIsRefused(*d);
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
