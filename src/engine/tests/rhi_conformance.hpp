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
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "engine/rhi/null/null_rhi.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"

namespace badlands::rhi::test {

// Captures everything logged during `fn` and returns it.
//
// Rule 1 says every failure path logs, and rule 9 says every feature is
// tested -- which together mean a refusal that logs needs a way to assert that
// it logged. Without this the diagnostics ARE the fix for a whole class of
// silent-fallback defects, and none of them can be tested, so the rule decays
// into a convention.
template <typename Fn>
inline std::string CaptureLog(Fn&& fn) {
  std::ostringstream oss;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
  auto capture = std::make_shared<spdlog::logger>("capture", sink);
  capture->set_level(spdlog::level::trace);

  auto previous = spdlog::default_logger();
  spdlog::set_default_logger(capture);
  try {
    fn();
  } catch (...) {
    spdlog::set_default_logger(previous);
    throw;
  }
  spdlog::set_default_logger(previous);
  return oss.str();
}

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
// A backend with no arm here would otherwise get an empty shader source, which
// compiles on Null, "succeeds" on a real backend often enough to pass
// REQUIRE(shader), and then shows up as a wrong centre pixel. Refuse loudly
// instead -- this is the file the DX12/HLSL arm goes in, and forgetting one of
// these functions must be impossible to miss.
inline const char* UnhandledBackend(const char* fn, BackendKind backend) {
  FAIL("rhi_conformance: " << fn << " has no source for backend "
                           << ToString(backend));
  return "";
}

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
  return UnhandledBackend("MinimalComputeSource", backend);
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
  return UnhandledBackend("MinimalGraphicsSource", backend);
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
  return UnhandledBackend("PullingGraphicsSource", backend);
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
  return UnhandledBackend("SamplingGraphicsSource", backend);
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

// A view must outlive Destroy() on its texture, because callers hold views as
// raw borrowed pointers (BindingEntry::texture_view, ColorAttachment::view) and
// the header promises destroyed-ness is a validation error rather than
// undefined behaviour. Metal freed its views here and Null did not, so the two
// backends disagreed about a documented contract with nothing to catch it.
inline void CheckViewsSurviveTextureDestroy(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 8, .height = 8,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled |
                                            TextureUsage::CopyDst,
                                   .label = "destroyed_tex"});
  REQUIRE(tex);
  ITextureView* view = tex->GetDefaultView();
  REQUIRE(view != nullptr);

  tex->Destroy();

  // The view object is still there to be asked; it just reports destroyed.
  CHECK(tex->IsDestroyed());
  CHECK(view->IsDestroyed());
  CHECK(view->GetTexture() == tex.get());
  CHECK(view->GetFormat() == Format::RGBA8Unorm);
}

// --- Creation-time refusals -------------------------------------------------
//
// These deliberately provoke errors, so they are NOT part of
// RunAllConformanceChecks -- that aggregate runs inside a validation scope and
// asserts it stays clean. Each suite calls them as its own TEST_CASE instead,
// which still gets them run against every backend (rule 6).

// Resources the public API cannot produce, so that the paths guarding against
// them are reachable. CreateBuffer always returns a shared_ptr and every view
// has an owning texture -- which is exactly why these refusals were untestable
// until something stood in for a backend that breaks the rule.
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

inline ComputePipelinePtr MakeTestPipeline(IRhiDevice& d) {
  auto module = d.CreateShaderModule(MinimalComputeSource(d.GetBackend()),
                                     MakeTestReflection(), "resolve_tests");
  return d.CreateComputePipeline({.shader = module.get(), .entry = "cs_main"});
}

// A table that cannot keep its resources alive must not EXIST. Building it and
// logging is still a use-after-free waiting to happen: rhi_types.hpp tells the
// caller it may drop its handle immediately, and a log line does not stop it.
inline void CheckUnretainableEntryIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  UnownedBuffer buf;

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = &buf}},
         .label = "unretainable"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("not shared_ptr-owned") != std::string::npos);
  // Names the table and the slot, or the message cannot be acted on.
  CHECK(log.find("unretainable") != std::string::npos);
  CHECK(log.find("slot 0") != std::string::npos);
}

inline void CheckOwnerlessViewIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  OwnerlessView view;

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 2, .kind = BindingKind::SampledTexture,
                      .texture_view = &view}},
         .label = "ownerless"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("no owning texture") != std::string::npos);
}

// Resolution happens once, at creation, so the record path cannot meet an
// unresolvable slot at all -- there is no later point where a backend has to
// choose between guessing an index and dropping the binding.
inline void CheckUnresolvableSlotIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "ubo"});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 9, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get()}},
         .label = "absent"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("slot 9") != std::string::npos);
  CHECK(log.find("absent from the pipeline's reflection") != std::string::npos);
}

inline void CheckMismatchedKindIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "ubo"});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    // Slot 0 is a UniformBuffer in reflection.
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                      .buffer = ubo.get()}},
         .label = "wrongkind"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("kind") != std::string::npos);
}

inline void CheckTableWithNoPipelineIsRefused(IRhiDevice& device) {
  auto ubo = device.CreateBuffer({.size = 64, .usage = BufferUsage::Uniform});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get()}},
         .label = "pipelineless"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("no pipeline") != std::string::npos);
}

// The paired green for every refusal above. Without it they would all pass
// just as well against a resolver that refuses everything.
//
// Binds all four declared slots, not a convenient subset: a partial table is
// itself a reported problem on a validated device, which would make "log is
// empty" fail for a reason that has nothing to do with resolution.
inline void CheckResolvableTableIsCreatedSilently(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "real_ubo"});
  auto ssbo = device.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Storage, .label = "real_ssbo"});
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled,
                                   .label = "real_tex"});
  auto samp = device.CreateSampler({.label = "real_samp"});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get()},
                     {.slot = 1, .kind = BindingKind::StorageBuffer,
                      .buffer = ssbo.get()},
                     {.slot = 2, .kind = BindingKind::SampledTexture,
                      .texture_view = tex->GetDefaultView()},
                     {.slot = 3, .kind = BindingKind::Sampler,
                      .sampler = samp.get()}},
         .label = "good"});
  });
  INFO(log);
  CHECK(table != nullptr);
  CHECK(log.empty());
}

// Destroy() keeps the view OBJECTS alive (callers hold them as raw borrowed
// pointers), so the cache outlives the GPU handle. A cache hit must not slip
// past the destroyed check -- populating the cache FIRST is the whole point of
// this case, and without it the bug hides.
inline void CheckCreateViewAfterDestroyIsRefused(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 8, .height = 8,
                                   .array_layers = 2,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled,
                                   .label = "dead_tex"});
  REQUIRE(tex);
  ITextureView* before = tex->GetDefaultView();  // populates the cache
  REQUIRE(before != nullptr);

  tex->Destroy();

  const std::string log = CaptureLog([&] {
    CHECK(tex->GetDefaultView() == nullptr);          // the CACHED range
    CHECK(tex->CreateView({.base_layer = 1}) == nullptr);  // an uncached one
  });
  INFO(log);
  CHECK(log.find("destroyed texture") != std::string::npos);

  // The view handed out before Destroy still exists and reports destroyed --
  // that contract is unchanged.
  CHECK(before->IsDestroyed());
}

// A binding table matching MakeTestReflection exactly, plus the resources it
// references and the transitions SetBindingTable will expect. Several tests
// need "a table that is entirely correct" as a starting point, so that a
// failure they DO provoke is the only thing reported.
struct FullBindingSet {
  BindingTablePtr table;
  BufferPtr ubo;
  BufferPtr ssbo;
  TexturePtr tex;
  SamplerPtr sampler;

  explicit operator bool() const { return table != nullptr; }

  void TransitionAll(ICommandEncoder& e) const {
    e.Transition(ubo.get(), ResourceState::ShaderRead);
    e.Transition(ssbo.get(), ResourceState::ShaderWrite);
    e.Transition(tex.get(), ResourceState::ShaderRead);
  }
};

inline FullBindingSet MakeFullBindingSet(IRhiDevice& d, IComputePipeline* pipe,
                                         const char* label) {
  FullBindingSet s;
  s.ubo = d.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Uniform, .label = "set_ubo"});
  s.ssbo = d.CreateBuffer(
      {.size = 64, .usage = BufferUsage::Storage, .label = "set_ssbo"});
  s.tex = d.CreateTexture({.width = 4, .height = 4,
                           .format = Format::RGBA8Unorm,
                           .usage = TextureUsage::Sampled,
                           .label = "set_tex"});
  s.sampler = d.CreateSampler({.label = "set_samp"});
  s.table = d.CreateBindingTable(
      {.compute_pipeline = pipe,
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = s.ubo.get()},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = s.ssbo.get()},
                   {.slot = 2, .kind = BindingKind::SampledTexture,
                    .texture_view = s.tex->GetDefaultView()},
                   {.slot = 3, .kind = BindingKind::Sampler,
                    .sampler = s.sampler.get()}},
       .label = label});
  return s;
}

// Frames advance, retire, and never exceed the pacing budget.
//
// Deliberately does NOT assert that frames overlap, because no backend can
// promise it here: Null-Immediate retires at EndFrame by construction, and
// Metal with trivial per-frame work usually completes before the next
// BeginFrame, so the assertion would pass or fail on scheduling luck. Overlap
// is proven where it can be proven deterministically -- under Null's manual
// retirement, which is what that mode exists for. See the pacing tests in
// rhi_tests.cpp.
inline void CheckFramesAdvanceAndPace(IRhiDevice& device) {
  const uint32_t limit = device.FramesInFlight();
  REQUIRE(limit >= 1);
  // Relative to where the device already is: the shared list runs every check
  // against ONE device, so an absolute frame count would only hold when this
  // case happens to run first.
  const uint64_t base = device.CurrentFrame();

  uint64_t peak_outstanding = 0;
  std::vector<uint64_t> outstanding_per_frame;
  constexpr int kFrames = 12;
  for (int i = 0; i < kFrames; ++i) {
    const uint64_t frame = device.BeginFrame();
    CHECK(frame == base + uint64_t(i) + 1);

    const uint64_t outstanding = device.CurrentFrame() - device.LastRetiredFrame();
    outstanding_per_frame.push_back(outstanding);
    peak_outstanding = std::max(peak_outstanding, outstanding);
    // The pacing guarantee: BeginFrame must not have returned while `limit`
    // frames were already outstanding.
    CHECK(outstanding <= limit);

    auto encoder = device.CreateCommandEncoder("frame");
    REQUIRE(encoder);
    encoder->Finish();
    device.Submit(*encoder);
    device.EndFrame();
  }

  // A table, not a visualiser -- the project's preference for showing data.
  std::string table = "frame:outstanding ";
  for (size_t i = 0; i < outstanding_per_frame.size(); ++i) {
    table += std::to_string(i + 1) + ":" + std::to_string(outstanding_per_frame[i]) + " ";
  }
  INFO(table);
  INFO("limit = " << limit << ", peak outstanding = " << peak_outstanding);

  device.WaitIdle();
  CHECK(device.CurrentFrame() == base + kFrames);
  CHECK(device.LastRetiredFrame() == device.CurrentFrame());
  CHECK(peak_outstanding >= 1);
}

// A frame that submits nothing still retires. A minimized or occluded window
// produces one of these every tick, and if it never returns its slot to the
// pacing budget the Nth such frame blocks in BeginFrame forever.
inline void CheckSkippedFramesStillRetire(IRhiDevice& device) {
  const uint32_t limit = device.FramesInFlight();
  const uint64_t base = device.CurrentFrame();
  // Deliberately more than the budget: without retirement this hangs.
  const uint32_t count = limit * 3 + 2;
  for (uint32_t i = 0; i < count; ++i) {
    device.BeginFrame();
    device.EndFrame();  // nothing submitted
  }
  CHECK(device.CurrentFrame() == base + count);
  CHECK(device.LastRetiredFrame() == device.CurrentFrame());
}

// Destroy() gives up the CALLER's claim, not the GPU's.
//
// Metal survives an immediate free because a command buffer retains what it
// references, so this mechanism could be entirely absent there and every
// pixel would still be correct -- which is exactly why PendingDeletions()
// exists to be asserted on. DX12 has no such safety net, and these assertions
// are its specification.
inline void CheckDestroyIsDeferredToFrameRetirement(IRhiDevice& device) {
  CHECK(device.PendingDeletions() == 0);

  device.BeginFrame();
  auto buf = device.CreateBuffer(
      {.size = 4096, .usage = BufferUsage::Storage, .label = "deferred"});
  REQUIRE(buf);

  buf->Destroy();
  // Observable at once...
  CHECK(buf->IsDestroyed());
  // ...but the memory is still held, because this frame may still be reading it.
  CHECK(device.PendingDeletions() == 1);

  // Idempotent, and must not enqueue a second time.
  buf->Destroy();
  CHECK(device.PendingDeletions() == 1);

  device.EndFrame();
  device.WaitIdle();
  CHECK(device.PendingDeletions() == 0);
}

// Destroyed with nothing in flight, there is nothing to wait for.
inline void CheckDestroyOutsideAFrameIsImmediate(IRhiDevice& device) {
  device.WaitIdle();
  const size_t before = device.PendingDeletions();
  auto buf = device.CreateBuffer(
      {.size = 256, .usage = BufferUsage::Storage, .label = "immediate"});
  buf->Destroy();
  CHECK(buf->IsDestroyed());
  CHECK(device.PendingDeletions() == before);
}

// Two resources cannot share tracked state just because the allocator reused
// an address. Ids are never reused; pointers are.
inline void CheckResourceIdsAreUnique(IRhiDevice& device) {
  uint64_t first_id = 0;
  const void* first_address = nullptr;
  {
    auto a = device.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
    REQUIRE(a);
    first_id = a->Id();
    first_address = a.get();
    CHECK(first_id != 0);
  }
  // `a` is gone; `b` may well land on its address.
  auto b = device.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
  REQUIRE(b);
  CHECK(b->Id() != first_id);
  INFO("reused address: " << (static_cast<const void*>(b.get()) == first_address));

  auto c = device.CreateTexture({.width = 4, .height = 4,
                                 .format = Format::RGBA8Unorm,
                                 .usage = TextureUsage::Sampled});
  CHECK(c->Id() != b->Id());
}

// The transient allocator: alignment, growth, the cap, and recycling.
inline void CheckFrameAllocatorBasics(IRhiDevice& device) {
  auto alloc = FrameAllocator::Create(device, {.block_size = 4096,
                                               .max_bytes_per_frame = 16384,
                                               .label = "basics"});
  REQUIRE(alloc);
  const uint64_t min_align = device.MinBufferOffsetAlignment();
  CHECK(min_align >= 1);
  CHECK((min_align & (min_align - 1)) == 0);  // power of two

  device.BeginFrame();
  alloc->BeginFrame(device.CurrentFrame());

  auto a = alloc->Allocate(100);
  REQUIRE(a);
  CHECK(a->buffer != nullptr);
  CHECK(a->offset % min_align == 0);
  CHECK(a->size == 100);

  // The next allocation must not overlap the first, and must still be aligned.
  auto b = alloc->Allocate(100);
  REQUIRE(b);
  CHECK(b->offset % min_align == 0);
  CHECK(b->offset >= a->offset + a->size);

  CHECK(alloc->BlocksThisFrame() == 1);
  device.EndFrame();
  device.WaitIdle();
}

// Refusals. A caller that treats a failed allocation as an empty one writes
// nothing and renders nothing, with no idea why.
inline void CheckFrameAllocatorRefusals(IRhiDevice& device) {
  auto alloc = FrameAllocator::Create(device, {.block_size = 4096,
                                               .max_bytes_per_frame = 8192,
                                               .label = "refusals"});
  REQUIRE(alloc);
  device.BeginFrame();
  alloc->BeginFrame(device.CurrentFrame());

  const std::string log = CaptureLog([&] {
    CHECK_FALSE(alloc->Allocate(0).has_value());        // zero bytes
    CHECK_FALSE(alloc->Allocate(64, 24).has_value());   // not a power of two
    CHECK_FALSE(alloc->Allocate(1024 * 1024).has_value());  // past the cap
  });
  INFO(log);
  CHECK(log.find("zero-byte") != std::string::npos);
  CHECK(log.find("power of two") != std::string::npos);
  CHECK(log.find("cap") != std::string::npos);

  device.EndFrame();
  device.WaitIdle();
}

// Growing past the block is a WARNING, not a failure: a heavy frame should
// still render. Exceeding the cap is an error, because an unbounded ring hides
// a leak instead of reporting one.
inline void CheckFrameAllocatorGrowsThenCaps(IRhiDevice& device) {
  auto alloc = FrameAllocator::Create(device, {.block_size = 1024,
                                               .max_bytes_per_frame = 4096,
                                               .label = "growth"});
  REQUIRE(alloc);
  device.BeginFrame();
  alloc->BeginFrame(device.CurrentFrame());

  const std::string log = CaptureLog([&] {
    // Fills the first block, then forces growth.
    CHECK(alloc->Allocate(1024).has_value());
    CHECK(alloc->Allocate(1024).has_value());
  });
  INFO(log);
  CHECK(alloc->BlocksThisFrame() == 2);
  CHECK(log.find("undersized") != std::string::npos);

  device.EndFrame();
  device.WaitIdle();
}

// A slot is reset only when its frame comes round again -- which BeginFrame
// has already blocked to guarantee. Two consecutive frames must not hand out
// the same bytes.
inline void CheckFrameAllocatorRecyclesPerSlot(IRhiDevice& device) {
  auto alloc = FrameAllocator::Create(device, {.block_size = 4096,
                                               .label = "recycle"});
  REQUIRE(alloc);

  device.BeginFrame();
  const uint64_t first_frame = device.CurrentFrame();
  alloc->BeginFrame(first_frame);
  auto first = alloc->Allocate(256);
  REQUIRE(first);
  device.EndFrame();

  device.BeginFrame();
  alloc->BeginFrame(device.CurrentFrame());
  auto second = alloc->Allocate(256);
  REQUIRE(second);
  device.EndFrame();

  // Different frames, so different slots: the same offset is fine, the same
  // BUFFER is not, or the second frame is overwriting bytes the first may
  // still be having read.
  if (device.FramesInFlight() > 1) {
    CHECK(first->buffer != second->buffer);
  }

  // Advance until the FIRST slot comes round again -- exactly
  // frames_in_flight frames later, whatever that is configured to.
  const uint64_t wrap_frame = first_frame + device.FramesInFlight();
  while (device.CurrentFrame() < wrap_frame) {
    device.BeginFrame();
    alloc->BeginFrame(device.CurrentFrame());
    if (device.CurrentFrame() < wrap_frame) device.EndFrame();
  }

  auto wrapped = alloc->Allocate(256);
  REQUIRE(wrapped);
  CHECK(wrapped->buffer == first->buffer);
  CHECK(wrapped->offset == first->offset);
  device.EndFrame();
  device.WaitIdle();
}

// A table with a dynamic offset, so per-frame data reaches a shader without
// one table per frame slot.
inline void CheckDynamicOffsetsReachTheBackend(IRhiDevice& device) {
  // The shared list runs every check against one device, so Find() would
  // otherwise return the FIRST SetBindingTable of the whole run rather than
  // this one -- the same accumulation that made an earlier aggregate fail
  // while its individual case passed.
  ResetLog(device);
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 4096, .usage = BufferUsage::Uniform, .label = "dyn_ubo"});
  auto ssbo = device.CreateBuffer(
      {.size = 4096, .usage = BufferUsage::Storage, .label = "dyn_ssbo"});
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled});
  auto samp = device.CreateSampler({});

  // Slots 0 and 1 are dynamic; 2 and 3 are not. The span is read in slot
  // order, so slot 0's offset comes first.
  auto table = device.CreateBindingTable(
      {.compute_pipeline = pipe.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = ubo.get(), .dynamic_offset = true},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = ssbo.get(), .dynamic_offset = true},
                   {.slot = 2, .kind = BindingKind::SampledTexture,
                    .texture_view = tex->GetDefaultView()},
                   {.slot = 3, .kind = BindingKind::Sampler,
                    .sampler = samp.get()}},
       .label = "dynamic"});
  REQUIRE(table);

  const uint64_t align = device.MinBufferOffsetAlignment();
  const uint32_t offsets[2] = {uint32_t(align), uint32_t(align * 2)};

  auto encoder = device.CreateCommandEncoder("dyn");
  encoder->Transition(ubo.get(), ResourceState::ShaderRead);
  encoder->Transition(ssbo.get(), ResourceState::ShaderWrite);
  encoder->Transition(tex.get(), ResourceState::ShaderRead);
  auto* pass = encoder->BeginComputePass("dyn");
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get(), offsets);
  pass->End();
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (auto* log = badlands::rhi::null::GetCommandLog(device)) {
    const auto* rec = log->Find(
        badlands::rhi::null::RecordedCommand::Kind::SetBindingTable);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->dynamic_offsets.size() == 2);
    CHECK(rec->dynamic_offsets[0] == offsets[0]);
    CHECK(rec->dynamic_offsets[1] == offsets[1]);
  }
}

// A table declaring more dynamic offsets than any target can carry is refused
// at creation, where it is cheap to fix.
inline void CheckTooManyDynamicOffsetsAreRefused(IRhiDevice& device) {
  // The reflection must DECLARE all of these slots. Using the shared
  // 4-slot reflection made this pass for the wrong reason -- slot 4 simply
  // failed to resolve -- which the red proof caught.
  ShaderReflection refl;
  for (uint32_t i = 0; i <= kMaxDynamicOffsetsPerTable; ++i) {
    refl.bindings.push_back({.group = 0, .slot = i,
                             .name = "b" + std::to_string(i),
                             .kind = BindingKind::UniformBuffer,
                             .location = {.space = 0, .index = i}});
  }
  ReflectedEntryPoint ep;
  ep.name = "cs_main";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 64;
  refl.entry_points.push_back(ep);

  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          refl, "manydyn");
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 256, .usage = BufferUsage::Uniform, .label = "u"});

  std::vector<BindingEntry> entries;
  for (uint32_t i = 0; i <= kMaxDynamicOffsetsPerTable; ++i) {
    entries.push_back({.slot = i, .kind = BindingKind::UniformBuffer,
                       .buffer = ubo.get(), .dynamic_offset = true});
  }

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable({.compute_pipeline = pipe.get(),
                                       .entries = entries,
                                       .label = "toomany"});
  });
  INFO(log);
  CHECK(table == nullptr);
  // Assert the REASON, not just the refusal -- otherwise any other resolution
  // failure satisfies this case.
  CHECK(log.find("cross-platform maximum") != std::string::npos);
}

// A dynamic offset re-points a buffer. Marking a texture or sampler dynamic
// would silently consume a value from the caller's span and shift every later
// offset onto the wrong binding.
inline void CheckDynamicOffsetOnANonBufferIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 2, .kind = BindingKind::SampledTexture,
                      .texture_view = tex->GetDefaultView(),
                      .dynamic_offset = true}},
         .label = "dyntex"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("only buffer bindings") != std::string::npos);
}

// Submitted work must retire rather than accumulate.
//
// Honest about its own strength: the load-bearing part is that InFlightCount()
// is PURE, so a backend cannot inherit a `0` that looks exactly like success --
// that is a compile-time guarantee, not something this case proves. What the
// case itself adds is cross-backend coverage of the contract, and coverage of
// the validation decorator's forwarding, which had none. The Metal-specific
// "submissions retire and stop accumulating" case is what actually watches the
// count fall.
inline void CheckSubmissionsRetire(IRhiDevice& device) {
  CHECK(device.InFlightCount() == 0);

  for (int i = 0; i < 4; ++i) {
    auto encoder = device.CreateCommandEncoder("retire");
    REQUIRE(encoder);
    encoder->Finish();
    device.Submit(*encoder);
  }
  device.WaitIdle();
  CHECK(device.InFlightCount() == 0);

  // And again, to catch a backend that only ever drains in WaitIdle: the
  // count must not have grown across rounds.
  for (int i = 0; i < 4; ++i) {
    auto encoder = device.CreateCommandEncoder("retire2");
    encoder->Finish();
    device.Submit(*encoder);
  }
  device.WaitIdle();
  CHECK(device.InFlightCount() == 0);
}

// TextureViewDesc's slicing fields were accepted and ignored: every backend
// handed back a whole-resource view whatever the caller asked for, and the view
// cache was keyed on (base_mip, base_layer) so two views differing only in
// COUNT collided. A descriptor field that is read and discarded is a trap with
// a delayed fuse -- the caller believes it is sampling layer 3.
inline void CheckSlicedViewsHonourTheirRange(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 8, .height = 8,
                                   .array_layers = 4,
                                   .mip_levels = 3,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled |
                                            TextureUsage::CopyDst,
                                   .label = "sliced"});
  REQUIRE(tex);

  // The default view resolves the "0 = all remaining" counts rather than
  // reporting 0, so a caller never has to know the texture to read them.
  ITextureView* whole = tex->GetDefaultView();
  REQUIRE(whole != nullptr);
  CHECK(whole->GetDesc().base_mip == 0);
  CHECK(whole->GetDesc().mip_count == 3);
  CHECK(whole->GetDesc().base_layer == 0);
  CHECK(whole->GetDesc().layer_count == 4);

  ITextureView* layer3 =
      tex->CreateView({.base_layer = 3, .layer_count = 1, .label = "layer3"});
  REQUIRE(layer3 != nullptr);
  CHECK(layer3->GetDesc().base_layer == 3);
  CHECK(layer3->GetDesc().layer_count == 1);
  CHECK(layer3->GetDesc().mip_count == 3);
  CHECK(layer3 != whole);

  // Same base, different count: a distinct view, not a cache hit on the first.
  ITextureView* layers2to3 = tex->CreateView({.base_layer = 2});
  ITextureView* layer2 = tex->CreateView({.base_layer = 2, .layer_count = 1});
  REQUIRE(layers2to3 != nullptr);
  REQUIRE(layer2 != nullptr);
  CHECK(layers2to3 != layer2);
  CHECK(layers2to3->GetDesc().layer_count == 2);
  CHECK(layer2->GetDesc().layer_count == 1);

  // Mip slicing resolves the same way.
  ITextureView* mip1 = tex->CreateView({.base_mip = 1});
  REQUIRE(mip1 != nullptr);
  CHECK(mip1->GetDesc().base_mip == 1);
  CHECK(mip1->GetDesc().mip_count == 2);

  // Identical descriptors still share one view.
  CHECK(tex->CreateView({.base_layer = 3, .layer_count = 1}) == layer3);
}

// A range the texture cannot satisfy is refused, not clamped. Clamping would
// hand back a whole-resource view for `base_layer = 9`, which reads as success
// and samples the wrong thing (rules 2 and 8).
inline void CheckOutOfRangeViewsAreRefused(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 8, .height = 8,
                                   .array_layers = 2,
                                   .mip_levels = 2,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled,
                                   .label = "small"});
  REQUIRE(tex);
  CHECK(tex->CreateView({.base_layer = 9}) == nullptr);
  CHECK(tex->CreateView({.base_mip = 5}) == nullptr);
  CHECK(tex->CreateView({.base_layer = 1, .layer_count = 4}) == nullptr);
  CHECK(tex->CreateView({.base_mip = 1, .mip_count = 3}) == nullptr);

  // Counts near UINT32_MAX -- what a caller lands on when a count is computed
  // by an underflowing subtraction. `base + count` is uint32 arithmetic and
  // WRAPS, so a naive check sums to something small and passes the very test
  // it has to fail.
  constexpr uint32_t kMax = 0xFFFFFFFFu;
  CHECK(tex->CreateView({.base_mip = 1, .mip_count = kMax}) == nullptr);
  CHECK(tex->CreateView({.base_layer = 1, .layer_count = kMax}) == nullptr);
  CHECK(tex->CreateView({.base_mip = 0, .mip_count = kMax}) == nullptr);
  // The in-range case still works, so the checks are not just refusing
  // everything.
  CHECK(tex->CreateView({.base_layer = 1, .layer_count = 1}) != nullptr);
}

// Binding tables keep their resources alive. Dropping the caller's last handle
// while a table still references it must not free the object underneath the
// table.
inline void CheckBindingTableRetainsItsResources(IRhiDevice& device) {
  auto module = device.CreateShaderModule(MinimalComputeSource(device.GetBackend()),
                                          MakeTestReflection(), "retain");
  auto pipe = device.CreateComputePipeline(
      {.shader = module.get(), .entry = "cs_main"});

  auto encoder = device.CreateCommandEncoder("retained");
  BindingTablePtr table;
  {
    auto ubo = device.CreateBuffer({.size = 64, .usage = BufferUsage::Uniform,
                                    .label = "retained_ubo"});
    auto ssbo = device.CreateBuffer({.size = 64, .usage = BufferUsage::Storage,
                                     .label = "retained_ssbo"});
    auto tex = device.CreateTexture({.width = 4, .height = 4,
                                     .format = Format::RGBA8Unorm,
                                     .usage = TextureUsage::Sampled,
                                     .label = "retained_tex"});
    auto samp = device.CreateSampler({.label = "retained_samp"});
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get()},
                     {.slot = 1, .kind = BindingKind::StorageBuffer,
                      .buffer = ssbo.get()},
                     {.slot = 2, .kind = BindingKind::SampledTexture,
                      .texture_view = tex->GetDefaultView()},
                     {.slot = 3, .kind = BindingKind::Sampler,
                      .sampler = samp.get()}},
         .label = "retaining"});
    REQUIRE(table);

    // Declared while the handles are still alive. The state tracker keys on the
    // resource, so this also proves it still resolves after the caller's handle
    // is gone -- the table is the only owner by the time the bind happens.
    encoder->Transition(ubo.get(), ResourceState::ShaderRead);
    encoder->Transition(ssbo.get(), ResourceState::ShaderWrite);
    encoder->Transition(tex.get(), ResourceState::ShaderRead);

    // Every caller handle goes out of scope here. Without retention inside the
    // table, everything below reads freed memory.
  }
  CHECK_FALSE(table->IsDestroyed());
  CHECK(table->GetGroup() == 0);

  // Actually USE it. Checking IsDestroyed()/GetGroup() alone passes vacuously,
  // because neither touches the entries -- and the entries are where a dangling
  // pointer lives. Binding walks every one of them.
  auto* pass = encoder->BeginComputePass("retained");
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->SetBindingTable(0, table.get());
  pass->Dispatch(1);
  pass->End();
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();
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
  auto* p1 = encoder->BeginRenderPass(clear_pass);
  REQUIRE(p1 != nullptr);
  p1->End();

  // Pass 2: load, draw nothing, store. The colour must survive.
  RenderPassDesc load_pass;
  load_pass.label = "load";
  load_pass.color_attachments.push_back(
      {.view = color->GetDefaultView(), .load_op = LoadOp::Load,
       .store_op = StoreOp::Store});
  auto* p2 = encoder->BeginRenderPass(load_pass);
  REQUIRE(p2 != nullptr);
  p2->End();

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
  CheckViewsSurviveTextureDestroy(device);
  CheckSlicedViewsHonourTheirRange(device);
  CheckOutOfRangeViewsAreRefused(device);
  CheckBindingTableRetainsItsResources(device);
  CheckSubmissionsRetire(device);
  CheckFramesAdvanceAndPace(device);
  CheckSkippedFramesStillRetire(device);
  CheckDestroyIsDeferredToFrameRetirement(device);
  CheckDestroyOutsideAFrameIsImmediate(device);
  CheckResourceIdsAreUnique(device);
  CheckFrameAllocatorBasics(device);
  CheckFrameAllocatorRefusals(device);
  CheckFrameAllocatorGrowsThenCaps(device);
  CheckFrameAllocatorRecyclesPerSlot(device);
  CheckDynamicOffsetsReachTheBackend(device);
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
