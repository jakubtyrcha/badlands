// RHI interface tests against the Null backend.
//
// No GPU is involved, so this suite runs anywhere and stays fast. It covers
// the interface contract, the shared conformance list, and the recording
// behaviour that makes Null usable as a test double.
//
// The same conformance list runs against Metal in badlands_rhi_metal_tests --
// see src/engine/tests/rhi_conformance.hpp for why that sharing matters.

#include <atomic>
#include <chrono>
#include <thread>

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
// Creation-time refusals. Shared with the Metal suite so the two backends
// cannot disagree about which tables and views are constructible (rule 6).
TEST_CASE("Null: an unretainable entry is refused at creation", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckUnretainableEntryIsRefused(*d);
}
TEST_CASE("Null: a view with no owning texture is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckOwnerlessViewIsRefused(*d);
}
TEST_CASE("Null: an unresolvable slot is refused at creation", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckUnresolvableSlotIsRefused(*d);
}
TEST_CASE("Null: a mismatched binding kind is refused at creation", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckMismatchedKindIsRefused(*d);
}
TEST_CASE("Null: a table with no pipeline is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTableWithNoPipelineIsRefused(*d);
}
TEST_CASE("Null: a resolvable table is created silently", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckResolvableTableIsCreatedSilently(*d);
}
TEST_CASE("Null: CreateView after Destroy is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckCreateViewAfterDestroyIsRefused(*d);
}
TEST_CASE("Null: a texture readback completes", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckReadbackCompletes(*d);
}
TEST_CASE("Null: a readback notifies exactly once", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckReadbackNotifiesExactlyOnce(*d);
}
TEST_CASE("Null: a readback of a multi-subresource view is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckReadbackRefusesMultiSubresourceView(*d);
}
TEST_CASE("Null: a readback of an uncopyable source is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckReadbackRefusesUncopyableSource(*d);
}
TEST_CASE("Null: cube textures and their views", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckCubeTexturesAndTheirViews(*d);
}
TEST_CASE("Null: cube dimension mismatches are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckCubeDimensionMismatchesAreRefused(*d);
}
TEST_CASE("Null: cube views on bad targets are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckCubeViewsOnBadTargetsAreRefused(*d);
}
TEST_CASE("Null: texture write bounds are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTextureWriteBoundsAreRefused(*d);
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
TEST_CASE("Null: frames advance and pace", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFramesAdvanceAndPace(*d);
}
TEST_CASE("Null: skipped frames still retire", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSkippedFramesStillRetire(*d);
}

TEST_CASE("Null: Destroy is deferred to frame retirement", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDestroyIsDeferredToFrameRetirement(*d);
}
TEST_CASE("Null: Destroy outside a frame is immediate", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDestroyOutsideAFrameIsImmediate(*d);
}
TEST_CASE("Null: resource ids are never reused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckResourceIdsAreUnique(*d);
}

TEST_CASE("Null: frame allocator basics", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFrameAllocatorBasics(*d);
}
TEST_CASE("Null: frame allocator refusals", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFrameAllocatorRefusals(*d);
}
TEST_CASE("Null: frame allocator grows then caps", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFrameAllocatorGrowsThenCaps(*d);
}
TEST_CASE("Null: frame allocator recycles per slot", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFrameAllocatorRecyclesPerSlot(*d);
}

TEST_CASE("Null: frame allocator survives growth failure", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFrameAllocatorSurvivesGrowthFailure(*d);
}
TEST_CASE("Null: wild buffer offsets are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckWildBufferOffsetsAreRefused(*d);
}

TEST_CASE("Null: an unaligned base offset is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckUnalignedBaseOffsetIsRefused(*d);
}
TEST_CASE("Null: a base offset past the buffer is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckBaseOffsetPastTheBufferIsRefused(*d);
}
TEST_CASE("Null: an unimplemented buffer_size is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckUnimplementedBufferSizeIsRefused(*d);
}
TEST_CASE("Null: WaitIdle does not retire the open frame", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckWaitIdleDoesNotRetireTheOpenFrame(*d);
}

TEST_CASE("Null: dynamic offsets reach the backend", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDynamicOffsetsReachTheBackend(*d);
}

TEST_CASE("Null: indirect dispatch reads its count", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDispatchIndirectReadsItsCount(*d);
}
TEST_CASE("Null: a zero indirect dispatch is allowed", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckZeroIndirectDispatchIsAllowed(*d);
}
TEST_CASE("Null: an indirect dispatch with no args is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckIndirectDispatchWithoutArgsIsRefused(*d);
}
TEST_CASE("Null: a draw with no pipeline is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDrawWithoutPipelineIsRefused(*d);
}
TEST_CASE("Null: out-of-range indirect args are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckOutOfRangeIndirectArgsAreRefused(*d);
}
TEST_CASE("Null: the feature query answers", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckFeatureQueryAnswers(*d);
}
TEST_CASE("Null: a mismatched blend state count is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckMismatchedBlendStateCountIsRefused(*d);
}
TEST_CASE("Null: an opaque pipeline needs no blend state", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckOpaquePipelineNeedsNoBlendState(*d);
}

TEST_CASE("Null: swapchain acquire/present cycle", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSwapchainAcquirePresentCycle(*d);
}
TEST_CASE("Null: swapchain skips when zero-sized", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSwapchainSkipsWhenZeroSized(*d);
}
TEST_CASE("Null: swapchain resize", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckSwapchainResize(*d);
}
TEST_CASE("Null: too many dynamic offsets are refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckTooManyDynamicOffsetsAreRefused(*d);
}
TEST_CASE("Null: a dynamic offset on a non-buffer is refused", "[rhi]") {
  auto d = MakeNull();
  rhitest::CheckDynamicOffsetOnANonBufferIsRefused(*d);
}

// --- Pacing, proven deterministically under manual retirement ---------------
//
// This is what Null's manual retirement mode is FOR. Immediate mode retires at
// EndFrame, and Metal with trivial work usually retires before the next frame
// begins, so neither can prove that frames overlap or that BeginFrame blocks.
// Driving retirement by hand makes the GPU timeline a thing the test controls,
// with no GPU and no scheduling luck.

TEST_CASE("Null: frames overlap up to the budget under manual retirement",
          "[rhi]") {
  auto d = CreateDevice({.backend = BackendKind::Null,
                         .frames_in_flight = 3,
                         .label = "pacing"});
  REQUIRE(d);
  badlands::rhi::null::SetRetirementMode(*d,
                                         badlands::rhi::null::RetirementMode::Manual);

  // Three frames begun and ended, none retired: all three are outstanding.
  for (int i = 0; i < 3; ++i) {
    d->BeginFrame();
    d->EndFrame();
  }
  CHECK(d->CurrentFrame() == 3);
  CHECK(d->LastRetiredFrame() == 0);
  CHECK(d->CurrentFrame() - d->LastRetiredFrame() == 3);

  // Retiring one lets exactly one more through.
  CHECK(badlands::rhi::null::RetireOldestFrame(*d));
  CHECK(d->LastRetiredFrame() == 1);
  d->BeginFrame();
  d->EndFrame();
  CHECK(d->CurrentFrame() - d->LastRetiredFrame() == 3);
}

TEST_CASE("Null: BeginFrame blocks until a frame retires", "[rhi]") {
  // The pacing guarantee itself. A budget that is never enforced looks
  // identical to one that is, until the GPU is a frame behind and the CPU has
  // overwritten a buffer it is still reading.
  auto d = CreateDevice({.backend = BackendKind::Null,
                         .frames_in_flight = 2,
                         .label = "blocking"});
  REQUIRE(d);
  badlands::rhi::null::SetRetirementMode(*d,
                                         badlands::rhi::null::RetirementMode::Manual);

  d->BeginFrame();
  d->EndFrame();
  d->BeginFrame();
  d->EndFrame();
  REQUIRE(d->CurrentFrame() - d->LastRetiredFrame() == 2);

  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    d->BeginFrame();  // budget is full -- must block
    returned.store(true, std::memory_order_release);
  });

  // Generous margin. The assertion is one-sided: if BeginFrame wrongly did not
  // block, `returned` is true here and the test fails. A slow machine only
  // makes it more likely to still be blocked, never less.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const bool still_blocked = !returned.load(std::memory_order_acquire);

  badlands::rhi::null::RetireOldestFrame(*d);
  waiter.join();

  CHECK(still_blocked);
  CHECK(returned.load(std::memory_order_acquire));
  d->EndFrame();
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

  // The pipeline is bound because a draw without one is now REFUSED on both
  // backends, not merely recorded. This case is about command ORDER, so it
  // states the precondition rather than leaning on Null having been lenient.
  auto raster = device->CreateRenderPipeline(
      {.vertex_shader = module.get(), .fragment_shader = module.get(),
       .color_formats = {Format::RGBA8Unorm}});
  RenderPassDesc desc;
  desc.color_attachments.push_back({.view = color->GetDefaultView()});
  auto* rp = encoder->BeginRenderPass(desc);
  rp->SetPipeline(raster.get());
  rp->Draw(3);
  rp->End();
  encoder->Finish();

  const auto& all = log->All();
  REQUIRE(all.size() >= 9);
  CHECK(all[0].kind == Kind::BeginComputePass);
  CHECK(all[1].kind == Kind::SetComputePipeline);
  CHECK(all[2].kind == Kind::Dispatch);
  CHECK(all[3].kind == Kind::EndComputePass);
  CHECK(all[4].kind == Kind::BeginRenderPass);
  CHECK(all[5].kind == Kind::SetRenderPipeline);
  CHECK(all[6].kind == Kind::Draw);
  CHECK(all[7].kind == Kind::EndRenderPass);
  CHECK(all[8].kind == Kind::Finish);
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
