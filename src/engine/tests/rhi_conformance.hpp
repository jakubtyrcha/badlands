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

#include <atomic>
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

// A device that starts refusing resource creation after N successes.
//
// Error paths are where the hardest defects live -- an allocator that leaves
// an index past the end, a loop that exits between BeginFrame and EndFrame --
// and none of them are reachable while every CreateBuffer succeeds. Nothing
// else in the suite can provoke a creation failure, so those paths were
// entirely untested.
//
// Forwards everything else, so a test can point it at either backend.
class FailAfterNDevice final : public IRhiDevice {
 public:
  FailAfterNDevice(IRhiDevice& inner, int successes_before_failure)
      : inner_(inner), remaining_(successes_before_failure) {}

  BackendKind GetBackend() const override { return inner_.GetBackend(); }
  IRhiDevice* Inner() override { return &inner_; }

  BufferPtr CreateBuffer(const BufferDesc& d) override {
    if (!Allow()) return nullptr;
    return inner_.CreateBuffer(d);
  }
  TexturePtr CreateTexture(const TextureDesc& d) override {
    if (!Allow()) return nullptr;
    return inner_.CreateTexture(d);
  }
  SamplerPtr CreateSampler(const SamplerDesc& d) override {
    return inner_.CreateSampler(d);
  }
  ShaderModulePtr CreateShaderModule(const std::string& s,
                                     const ShaderReflection& r,
                                     const std::string& l) override {
    return inner_.CreateShaderModule(s, r, l);
  }
  RenderPipelinePtr CreateRenderPipeline(const RenderPipelineDesc& d) override {
    return inner_.CreateRenderPipeline(d);
  }
  ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& d) override {
    return inner_.CreateComputePipeline(d);
  }
  BindingTablePtr CreateBindingTable(const BindingTableDesc& d) override {
    return inner_.CreateBindingTable(d);
  }
  SwapchainPtr CreateSwapchain(const SwapchainDesc& d) override {
    return inner_.CreateSwapchain(d);
  }
  std::unique_ptr<ICommandEncoder> CreateCommandEncoder(
      const std::string& l) override {
    return inner_.CreateCommandEncoder(l);
  }
  void Submit(ICommandEncoder& e) override { inner_.Submit(e); }
  TextureReadbackPtr ReadTexture(ICommandEncoder& e, ITextureView* src) override {
    return inner_.ReadTexture(e, src);
  }
  void WaitIdle() override { inner_.WaitIdle(); }
  size_t InFlightCount() override { return inner_.InFlightCount(); }
  uint64_t BeginFrame() override { return inner_.BeginFrame(); }
  void EndFrame() override { inner_.EndFrame(); }
  uint64_t CurrentFrame() const override { return inner_.CurrentFrame(); }
  uint64_t LastRetiredFrame() const override {
    return inner_.LastRetiredFrame();
  }
  uint32_t FramesInFlight() const override { return inner_.FramesInFlight(); }
  size_t PendingDeletions() const override {
    return inner_.PendingDeletions();
  }
  uint64_t MinBufferOffsetAlignment() const override {
    return inner_.MinBufferOffsetAlignment();
  }
  bool Supports(DeviceFeature f) const override { return inner_.Supports(f); }
  void BeginValidationScope() override { inner_.BeginValidationScope(); }
  std::optional<ValidationReport> EndValidationScope() override {
    return inner_.EndValidationScope();
  }
  bool IsValidationEnabled() const override {
    return inner_.IsValidationEnabled();
  }

 private:
  bool Allow() {
    if (remaining_ <= 0) return false;
    --remaining_;
    return true;
  }

  IRhiDevice& inner_;
  int remaining_;
};

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

// --- Cube textures ----------------------------------------------------------
//
// A cube is not an array that happens to have six slices: it is sampled by
// direction and the hardware filters across face boundaries. What makes one
// USABLE is the PAIR of views below -- render into one face through a Tex2D
// view, sample the whole thing through a Cube view -- so a check that only
// created the texture would prove nothing about the case IBL needs.

inline void CheckCubeTexturesAndTheirViews(IRhiDevice& device) {
  auto cube = device.CreateTexture({.width = 16, .height = 16,
                                    .array_layers = 6,
                                    .mip_levels = 3,
                                    .format = Format::RGBA16Float,
                                    .usage = TextureUsage::Sampled |
                                             TextureUsage::RenderTarget |
                                             TextureUsage::CopyDst,
                                    .dimension = TextureDimension::Cube,
                                    .label = "env_cube"});
  REQUIRE(cube);
  CHECK(cube->GetArrayLayers() == 6);
  CHECK(cube->GetMipLevels() == 3);

  // The SAMPLING view: all six faces, all mips, read by direction.
  ITextureView* sampled = cube->CreateView(
      {.dimension = TextureViewDimension::Cube, .label = "env_cube.sample"});
  REQUIRE(sampled != nullptr);
  CHECK(sampled->GetDesc().layer_count == 6);
  CHECK(sampled->GetDesc().mip_count == 3);
  CHECK(sampled->GetDesc().dimension == TextureViewDimension::Cube);

  // The PREFILTER TARGET view: one face, one mip, as a flat 2D image.
  ITextureView* face =
      cube->CreateView({.base_mip = 1,
                        .mip_count = 1,
                        .base_layer = 3,
                        .layer_count = 1,
                        .dimension = TextureViewDimension::Tex2D,
                        .label = "env_cube.f3m1"});
  REQUIRE(face != nullptr);
  CHECK(face->GetDesc().base_mip == 1);
  CHECK(face->GetDesc().base_layer == 3);
  CHECK(face->GetDesc().layer_count == 1);
  CHECK(face->GetDesc().dimension == TextureViewDimension::Tex2D);

  // Two views over the SAME range differing only in dimension must not collide
  // in the view cache. The cache key already had to grow once, when two views
  // differing only in COUNT silently returned each other -- dimension is the
  // same defect with a new field.
  ITextureView* as_array =
      cube->CreateView({.dimension = TextureViewDimension::Tex2DArray});
  REQUIRE(as_array != nullptr);
  CHECK(as_array != sampled);
  CHECK(as_array->GetDesc().dimension == TextureViewDimension::Tex2DArray);
  CHECK(sampled->GetDesc().dimension == TextureViewDimension::Cube);
}

// --- Async texture readback -------------------------------------------------
//
// The shared contract is the COMPLETION PROTOCOL, not the texels. Null has a
// frame model but no GPU and no texel storage, so what both backends can
// promise is: not ready before the work is submitted, ready once waited on, and
// a callback that fires exactly once. The values themselves are asserted in the
// Metal suite, where there is something to assert.
//
// Deliberately NOT asserted: that a readback is un-ready immediately after
// Submit. Metal usually has not finished; Null completes at once. Requiring
// either would be requiring a timing difference, which is how rule 6 gets
// broken by a test rather than by a backend.

inline void CheckReadbackCompletes(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled |
                                            TextureUsage::CopyDst |
                                            TextureUsage::CopySrc,
                                   .label = "readback_src"});
  REQUIRE(tex);
  std::vector<uint8_t> texels(4 * 4 * 4, 0x5a);
  tex->Write(0, 0, AsBytes(texels));

  device.BeginFrame();
  auto encoder = device.CreateCommandEncoder("readback");
  REQUIRE(encoder);
  // THE SAME VIEW a shader binding would take. One abstraction names a
  // subresource in this RHI, and passing the object you would bind is what
  // makes "read back what the shader sees" a statement rather than a hope.
  auto* view = tex->CreateView({.mip_count = 1, .layer_count = 1});
  REQUIRE(view);
  auto readback = device.ReadTexture(*encoder, view);
  REQUIRE(readback);
  CHECK(readback->GetWidth() == 4);
  CHECK(readback->GetHeight() == 4);
  CHECK(readback->GetFormat() == Format::RGBA8Unorm);

  // Before anything is submitted there is nothing to be ready for. Both
  // backends can promise this one.
  CHECK_FALSE(readback->IsReady());

  encoder->Finish();
  device.Submit(*encoder);

  // Wait returns true and, having returned, the data has landed. That ordering
  // is the whole contract -- not "IsReady eventually flips".
  REQUIRE(readback->Wait());
  CHECK(readback->IsReady());
  CHECK(readback->Data().size() == 4 * 4 * 4);
  device.EndFrame();
}

inline void CheckReadbackNotifiesExactlyOnce(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 2, .height = 2,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::CopyDst |
                                            TextureUsage::CopySrc,
                                   .label = "notify_src"});
  REQUIRE(tex);

  device.BeginFrame();
  auto encoder = device.CreateCommandEncoder("notify");
  auto* view = tex->CreateView({.mip_count = 1, .layer_count = 1});
  REQUIRE(view);
  auto readback = device.ReadTexture(*encoder, view);
  REQUIRE(readback);

  std::atomic<int> fired{0};
  readback->OnComplete([&fired] { ++fired; });

  encoder->Finish();
  device.Submit(*encoder);
  REQUIRE(readback->Wait());

  // FIRED BY THE TIME WAIT RETURNS. A callback that arrives later is a
  // callback a caller cannot rely on: --screenshot waits and then exits, so a
  // completion delivered after that is a completion delivered never.
  CHECK(fired.load() == 1);

  // And registering after the fact runs immediately rather than never -- the
  // case a caller hits when the copy finished while they were doing something
  // else, and the one that silently does nothing if the backend only ever
  // notifies from its completion handler.
  std::atomic<int> late{0};
  readback->OnComplete([&late] { ++late; });
  CHECK(late.load() == 1);
  CHECK(fired.load() == 1);  // and the first one is not called again
  device.EndFrame();
}

inline void CheckReadbackRefusesUncopyableSource(IRhiDevice& device) {
  // No CopySrc: the copy could never be encoded, so the readback must not
  // exist rather than fail at submit (rule 13).
  auto tex = device.CreateTexture({.width = 4, .height = 4,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled,
                                   .label = "not_copyable"});
  REQUIRE(tex);
  device.BeginFrame();
  auto encoder = device.CreateCommandEncoder("refuse");

  auto* view = tex->CreateView({.mip_count = 1, .layer_count = 1});
  REQUIRE(view);
  const std::string log = CaptureLog([&] {
    CHECK(device.ReadTexture(*encoder, view) == nullptr);
    CHECK(device.ReadTexture(*encoder, nullptr) == nullptr);
  });
  INFO(log);
  CHECK(log.find("not_copyable") != std::string::npos);
  device.EndFrame();
}

// A readback produces ONE tightly packed image, so a view spanning several mips
// or layers has no single answer. Reading its base anyway would be the
// accepted-and-ignored trap (rule 4) -- the caller would get a correct-looking
// image of the wrong extent and nothing would say so.
//
// This refusal only became expressible once ReadTexture took a VIEW: a
// (texture, mip, layer) triple cannot describe a multi-subresource request, so
// there was nothing to refuse.
inline void CheckReadbackRefusesMultiSubresourceView(IRhiDevice& device) {
  auto tex = device.CreateTexture({.width = 8, .height = 8,
                                   .array_layers = 4,
                                   .mip_levels = 3,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::CopySrc |
                                            TextureUsage::CopyDst,
                                   .dimension = TextureDimension::Tex2DArray,
                                   .label = "multi_sub"});
  REQUIRE(tex);
  device.BeginFrame();
  auto encoder = device.CreateCommandEncoder("multi");

  // Whole-resource: three mips and four layers.
  auto* whole = tex->GetDefaultView();
  // One layer but every mip.
  auto* all_mips = tex->CreateView({.base_layer = 1, .layer_count = 1});
  // One mip but every layer.
  auto* all_layers = tex->CreateView({.base_mip = 1, .mip_count = 1});

  const std::string log = CaptureLog([&] {
    CHECK(device.ReadTexture(*encoder, whole) == nullptr);
    CHECK(device.ReadTexture(*encoder, all_mips) == nullptr);
    CHECK(device.ReadTexture(*encoder, all_layers) == nullptr);
  });
  INFO(log);
  CHECK(log.find("ONE subresource") != std::string::npos);

  // And the single-subresource view IS accepted, so the refusal is about the
  // range rather than about the texture.
  auto* one = tex->CreateView({.base_mip = 2, .mip_count = 1,
                               .base_layer = 3, .layer_count = 1});
  REQUIRE(one);
  auto rb = device.ReadTexture(*encoder, one);
  REQUIRE(rb);
  CHECK(rb->GetWidth() == 2);   // 8 >> 2
  CHECK(rb->GetHeight() == 2);
  device.EndFrame();
}

// --- Creation-time refusals -------------------------------------------------
//
// These deliberately provoke errors, so they are NOT part of
// RunAllConformanceChecks -- that aggregate runs inside a validation scope and
// asserts it stays clean. Each suite calls them as its own TEST_CASE instead,
// which still gets them run against every backend (rule 6).

// The four ways a dimension can contradict the layers beside it. All refused at
// CREATION, not by the validation decorator: an object that cannot be encoded
// must not exist in a release build either (rule 13).
inline void CheckCubeDimensionMismatchesAreRefused(IRhiDevice& device) {
  const std::string log = CaptureLog([&] {
    // Five faces is not a cube.
    CHECK(device.CreateTexture({.width = 16, .height = 16,
                                .array_layers = 5,
                                .format = Format::RGBA16Float,
                                .usage = TextureUsage::Sampled,
                                .dimension = TextureDimension::Cube,
                                .label = "five_faces"}) == nullptr);
    // Nor is a non-square one: a face is square by construction, and a
    // rectangular "cube" samples to garbage rather than failing.
    CHECK(device.CreateTexture({.width = 16, .height = 8,
                                .array_layers = 6,
                                .format = Format::RGBA16Float,
                                .usage = TextureUsage::Sampled,
                                .dimension = TextureDimension::Cube,
                                .label = "oblong_cube"}) == nullptr);
    // A plain 2D texture has exactly one layer. Before the dimension field this
    // silently became an array, which is the ambiguity it exists to remove.
    CHECK(device.CreateTexture({.width = 16, .height = 16,
                                .array_layers = 3,
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled,
                                .dimension = TextureDimension::Tex2D,
                                .label = "layered_2d"}) == nullptr);
  });
  INFO(log);
  CHECK(log.find("five_faces") != std::string::npos);
  CHECK(log.find("oblong_cube") != std::string::npos);
  CHECK(log.find("layered_2d") != std::string::npos);
}

inline void CheckCubeViewsOnBadTargetsAreRefused(IRhiDevice& device) {
  auto flat = device.CreateTexture({.width = 16, .height = 16,
                                    .array_layers = 6,
                                    .format = Format::RGBA16Float,
                                    .usage = TextureUsage::Sampled,
                                    .dimension = TextureDimension::Tex2DArray,
                                    .label = "six_layer_array"});
  REQUIRE(flat);
  auto cube = device.CreateTexture({.width = 16, .height = 16,
                                    .array_layers = 6,
                                    .format = Format::RGBA16Float,
                                    .usage = TextureUsage::Sampled,
                                    .dimension = TextureDimension::Cube,
                                    .label = "real_cube"});
  REQUIRE(cube);

  const std::string log = CaptureLog([&] {
    // Six layers is NECESSARY but not sufficient -- the underlying texture must
    // have been created as a cube, because that is what the backend encodes.
    CHECK(flat->CreateView({.dimension = TextureViewDimension::Cube}) ==
          nullptr);
    // A cube view must cover six layers, so a base that leaves fewer behind it
    // cannot produce one.
    CHECK(cube->CreateView({.base_layer = 2,
                            .dimension = TextureViewDimension::Cube}) ==
          nullptr);
    // ... and neither can an explicit count that is not six.
    CHECK(cube->CreateView({.layer_count = 3,
                            .dimension = TextureViewDimension::Cube}) ==
          nullptr);
  });
  INFO(log);
  CHECK(log.find("six_layer_array") != std::string::npos);
  CHECK(log.find("real_cube") != std::string::npos);
}

// Write() validated the data SIZE and nothing else, so a mip or layer past the
// end reached the backend -- on Metal, a replaceRegion into a slice that does
// not exist. Invisible while every texture had one layer; a cube has six.
//
// Null did not check even the size, which is rule 6 all over again: the two
// backends disagreed about a documented contract and the shared suite could not
// see it because nothing asked.
inline void CheckTextureWriteBoundsAreRefused(IRhiDevice& device) {
  auto cube = device.CreateTexture({.width = 4, .height = 4,
                                    .array_layers = 6,
                                    .mip_levels = 2,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::Sampled |
                                             TextureUsage::CopyDst,
                                    .dimension = TextureDimension::Cube,
                                    .label = "write_bounds_cube"});
  REQUIRE(cube);
  std::vector<uint8_t> mip0(4 * 4 * 4, 0x7f);

  const std::string log = CaptureLog([&] {
    cube->Write(0, 6, AsBytes(mip0));  // layer past the last face
    cube->Write(2, 0, AsBytes(mip0));  // mip past the last level
    std::vector<uint8_t> stub(8, 0);
    cube->Write(0, 0, AsBytes(stub));  // short data for a 4x4 RGBA8 level
  });
  INFO(log);
  CHECK(log.find("layer 6") != std::string::npos);
  CHECK(log.find("mip 2") != std::string::npos);
  CHECK(log.find("short") != std::string::npos);

  // A write that IS in bounds still goes through, so the guard refuses rather
  // than disabling the path.
  const std::string clean =
      CaptureLog([&] { cube->Write(1, 5, AsBytes(mip0)); });
  CHECK(clean.empty());
}

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
                                   .dimension = TextureDimension::Tex2DArray,
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

  // The SAME buffer, at a DIFFERENT offset. That is the whole point of the
  // partitioned primary: a binding table is immutable and names one buffer, so
  // an allocator handing out a different buffer per frame could not back a
  // per-frame binding at all. Distinct offsets are what stop frame N
  // overwriting what N-1 is still being read from.
  CHECK(first->buffer == second->buffer);
  if (device.FramesInFlight() > 1) {
    CHECK(first->offset != second->offset);
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
  // Back on the first slot, so back at its base -- the slice is reused only
  // now, once the frame that held it has retired.
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

// The acquire/present cycle, headless. A swapchain with no native window
// hands out its own images, which is what makes this state machine testable
// on every backend with no display attached.
inline void CheckSwapchainAcquirePresentCycle(IRhiDevice& device) {
  auto sc = device.CreateSwapchain({.width = 64, .height = 32,
                                    .format = Format::BGRA8Unorm,
                                    .label = "headless"});
  REQUIRE(sc);
  CHECK(sc->GetWidth() == 64);
  CHECK(sc->GetHeight() == 32);

  // More cycles than the pool is deep, so an implementation that leaks a
  // backbuffer per frame runs out.
  std::vector<ITextureView*> seen;
  for (uint32_t i = 0; i < device.FramesInFlight() * 3; ++i) {
    device.BeginFrame();
    auto frame = sc->Acquire();
    REQUIRE(frame.status == AcquireStatus::Ok);
    REQUIRE(frame.view != nullptr);
    seen.push_back(frame.view);
    sc->Present();
    device.EndFrame();
  }
  device.WaitIdle();

  // A real pool rotates. Handing back one image forever would pass every
  // other assertion here.
  if (device.FramesInFlight() > 1) {
    CHECK(seen[0] != seen[1]);
  }
}

// A zero-sized surface is what a minimized window reports. Skip, never a 0x0
// texture and never an error.
inline void CheckSwapchainSkipsWhenZeroSized(IRhiDevice& device) {
  auto sc = device.CreateSwapchain({.width = 0, .height = 0,
                                    .label = "minimized"});
  REQUIRE(sc);
  device.BeginFrame();
  auto frame = sc->Acquire();
  CHECK(frame.status == AcquireStatus::Skip);
  CHECK(frame.view == nullptr);
  device.EndFrame();

  // And it recovers when the window comes back.
  sc->Resize(32, 32);
  device.BeginFrame();
  auto back = sc->Acquire();
  CHECK(back.status == AcquireStatus::Ok);
  CHECK(back.view != nullptr);
  sc->Present();
  device.EndFrame();
  device.WaitIdle();
}

// Resize hands out differently-sized backbuffers, and the old ones go through
// deferred deletion rather than being freed under a frame still reading them.
inline void CheckSwapchainResize(IRhiDevice& device) {
  auto sc = device.CreateSwapchain({.width = 64, .height = 64,
                                    .label = "resizing"});
  REQUIRE(sc);

  device.BeginFrame();
  auto before = sc->Acquire();
  REQUIRE(before.status == AcquireStatus::Ok);
  REQUIRE(before.view != nullptr);
  REQUIRE(before.view->GetTexture() != nullptr);
  CHECK(before.view->GetTexture()->GetWidth() == 64);
  sc->Present();
  device.EndFrame();

  device.BeginFrame();
  sc->Resize(128, 96);
  CHECK(sc->GetWidth() == 128);
  CHECK(sc->GetHeight() == 96);
  auto after = sc->Acquire();
  REQUIRE(after.status == AcquireStatus::Ok);
  REQUIRE(after.view != nullptr);
  REQUIRE(after.view->GetTexture() != nullptr);
  CHECK(after.view->GetTexture()->GetWidth() == 128);
  CHECK(after.view->GetTexture()->GetHeight() == 96);
  sc->Present();
  device.EndFrame();
  device.WaitIdle();
}

// The colour space a swapchain was asked for must be the one it reports, and
// the default must stay untagged sRGB -- every call site written before colour
// spaces existed relies on that being a no-op.
inline void CheckSwapchainReportsItsColorSpace(IRhiDevice& device) {
  auto plain = device.CreateSwapchain({.width = 32, .height = 32,
                                       .label = "default_cs"});
  REQUIRE(plain);
  CHECK(plain->GetColorSpace() == ColorSpace::Srgb);

  auto p3 = device.CreateSwapchain({.width = 32,
                                    .height = 32,
                                    .color_space = ColorSpace::DisplayP3,
                                    .label = "p3"});
  REQUIRE(p3);
  CHECK(p3->GetColorSpace() == ColorSpace::DisplayP3);

  // Extended-range headless is legal on both backends: nothing is presented,
  // so there is no transfer for a compositor to get wrong. This is the case
  // the two-sink test depends on -- it is how the EDR path is reachable on a
  // machine with no HDR display.
  auto edr = device.CreateSwapchain(
      {.width = 32,
       .height = 32,
       .format = Format::RGBA16Float,
       .color_space = ColorSpace::ExtendedLinearDisplayP3,
       .label = "edr"});
  REQUIRE(edr);
  CHECK(edr->GetFormat() == Format::RGBA16Float);
  CHECK(edr->GetColorSpace() == ColorSpace::ExtendedLinearDisplayP3);
}

// An extended-range surface ON A REAL WINDOW with no colour space has no
// defined transfer, so it must be refused at creation rather than presented and
// guessed at. Both backends share ValidateSwapchainDesc precisely so they
// cannot disagree about this (rule 13).
//
// A fake non-null window handle is enough FOR A REFUSAL, because the refusal
// happens before either backend touches the handle. That keeps the check on
// Null too, where a real window is impossible -- and a refusal only Metal could
// test is a refusal Null could quietly stop performing.
//
// IT IS NOT ENOUGH FOR AN ACCEPTANCE. Metal's swapchain really does message the
// handle (`layer_.device = ...`), so a desc that passes validation with a fake
// one segfaults -- which is exactly what the first version of this check did.
// So the "it is the combination, not the format" half of the claim is made the
// only way it can be: by CheckSwapchainReportsItsColorSpace accepting the same
// extended-range format with no window at all.
inline void CheckUntaggedExtendedRangeSurfaceIsRefused(IRhiDevice& device) {
  int not_a_window = 0;
  auto sc = device.CreateSwapchain(
      {.native_window = &not_a_window,
       .width = 32,
       .height = 32,
       .format = Format::RGBA16Float,
       .color_space = ColorSpace::Srgb,
       .label = "untagged_float"});
  CHECK(sc == nullptr);

  // AND THE MIRROR IMAGE. An 8-bit surface tagged extended-linear has the
  // compositor read sRGB-encoded bytes as linear intensities -- wrong in the
  // same way and for the same reason, and the validator originally checked only
  // one direction.
  auto linear_8bit = device.CreateSwapchain(
      {.native_window = &not_a_window,
       .width = 32,
       .height = 32,
       .format = Format::BGRA8Unorm,
       .color_space = ColorSpace::ExtendedLinearDisplayP3,
       .label = "linear_8bit"});
  CHECK(linear_8bit == nullptr);
}

// A swapchain's FORMAT is fixed once it has been reported. Resize may re-tag
// the surface, but it must never swap the format underneath a caller who has
// already built pipelines against it -- there is no path that rebuilds them,
// and a pipeline whose colour format does not match its attachment does not
// fail loudly: it renders a wrong image.
inline void CheckResizeKeepsTheFormat(IRhiDevice& device) {
  for (Format f : {Format::BGRA8Unorm, Format::RGBA8Unorm}) {
    auto sc = device.CreateSwapchain(
        {.width = 32, .height = 32, .format = f, .label = "stable_format"});
    REQUIRE(sc);
    const Format before = sc->GetFormat();
    const ColorSpace cs_before = sc->GetColorSpace();
    sc->Resize(64, 48);
    CHECK(sc->GetFormat() == before);
    CHECK(sc->GetColorSpace() == cs_before);
    sc->Resize(0, 0);  // minimize, the other size a real window reaches
    CHECK(sc->GetFormat() == before);
    sc->Resize(32, 32);
    CHECK(sc->GetFormat() == before);
  }
}

// A wild offset must be refused, not wrapped past the guard.
//
// `offset + size > capacity` is unsigned arithmetic: an offset near
// UINT64_MAX sums to something small, passes, and memcpys through a wild
// pointer. These are the two functions in the whole RHI that actually write
// memory, and they were the ones still on the addition form after rule 8 was
// written.
inline void CheckWildBufferOffsetsAreRefused(IRhiDevice& device) {
  auto buf = device.CreateBuffer({.size = 256,
                                  .usage = BufferUsage::CopyDst |
                                           BufferUsage::MapRead,
                                  .label = "wild"});
  REQUIRE(buf);

  std::vector<uint8_t> known(64, 0xAB);
  buf->Write(0, AsBytes(known));

  std::vector<uint8_t> one(1, 0xFF);
  std::vector<uint8_t> out(1, 0);
  const std::string log = CaptureLog([&] {
    buf->Write(~uint64_t(0), AsBytes(one));       // wraps to 0
    buf->Write(~uint64_t(0) - 4, AsBytes(one));   // wraps just past
    CHECK_FALSE(buf->Read(~uint64_t(0), out));
  });
  INFO(log);
  CHECK(log.find("runs past") != std::string::npos);

  // The refusals wrote nothing: the known bytes are intact.
  std::vector<uint8_t> check(64, 0);
  REQUIRE(buf->Read(0, check));
  CHECK(check == known);
}

// WaitIdle drains submitted work; it does not END the open frame. Declaring
// it retired let a Destroy()ed handle be released while the caller could still
// bind it into that very frame -- and Null, whose WaitIdle only retires ended
// frames, gave the opposite answer to the same call.
inline void CheckWaitIdleDoesNotRetireTheOpenFrame(IRhiDevice& device) {
  device.WaitIdle();
  device.BeginFrame();
  device.WaitIdle();  // nothing submitted, but the frame is still OPEN

  auto buf = device.CreateBuffer(
      {.size = 1024, .usage = BufferUsage::Storage, .label = "open_frame"});
  REQUIRE(buf);
  buf->Destroy();

  // Still held, because this frame has not retired and the caller could have
  // bound it before destroying it.
  CHECK(device.PendingDeletions() == 1);

  device.EndFrame();
  device.WaitIdle();
  CHECK(device.PendingDeletions() == 0);
}

// A transient allocation that cannot grow must refuse and leave the allocator
// usable. The caller's contract is that a refused allocation skips one draw
// and the frame carries on, so the very next Allocate has to be safe.
inline void CheckFrameAllocatorSurvivesGrowthFailure(IRhiDevice& device) {
  // Exactly one success: the shared primary. Every growth block after that
  // fails, which is the path under test.
  FailAfterNDevice failing(device, 1);
  auto alloc = FrameAllocator::Create(failing, {.block_size = 1024,
                                                .max_bytes_per_frame = 65536,
                                                .label = "growthfail"});
  REQUIRE(alloc);

  device.BeginFrame();
  alloc->BeginFrame(device.CurrentFrame());

  const std::string log = CaptureLog([&] {
    CHECK(alloc->Allocate(1024).has_value());          // fills the block
    CHECK_FALSE(alloc->Allocate(512).has_value());     // growth fails
    // The allocator must still be in a usable state. Un-fixed, block_index was
    // left one past the end and this read off the end of the vector.
    CHECK_FALSE(alloc->Allocate(512).has_value());
    CHECK_FALSE(alloc->Allocate(64).has_value());
  });
  INFO(log);

  device.EndFrame();
  device.WaitIdle();
}

// NOT in RunAllConformanceChecks: like the other creation-time refusals these
// provoke validation reports on purpose, and the aggregate asserts a clean
// scope. Each suite calls them as its own TEST_CASE instead.
//
// The base offset is fixed for the table's life, so it is checked when the
// table is built. Only the DYNAMIC part was ever checked, so an unaligned
// base plus a correctly aligned dynamic offset produced a final address that
// satisfied neither.
inline void CheckUnalignedBaseOffsetIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 1024, .usage = BufferUsage::Uniform, .label = "u"});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get(), .buffer_offset = 1}},
         .label = "unaligned_base"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("not a multiple") != std::string::npos);
}

inline void CheckBaseOffsetPastTheBufferIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 256, .usage = BufferUsage::Uniform, .label = "small"});
  const uint64_t align = device.MinBufferOffsetAlignment();

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get(),
                      .buffer_offset = align * 100}},
         .label = "past_end"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("past the end") != std::string::npos);
}

// buffer_size is read by no backend, so accepting it would be a bound the
// caller believes in and does not have (rule 4).
inline void CheckUnimplementedBufferSizeIsRefused(IRhiDevice& device) {
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);
  auto ubo = device.CreateBuffer(
      {.size = 1024, .usage = BufferUsage::Uniform, .label = "u"});

  BindingTablePtr table;
  const std::string log = CaptureLog([&] {
    table = device.CreateBindingTable(
        {.compute_pipeline = pipe.get(),
         .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                      .buffer = ubo.get(), .buffer_size = 64}},
         .label = "sized"});
  });
  INFO(log);
  CHECK(table == nullptr);
  CHECK(log.find("no backend implements") != std::string::npos);
}

// A dispatch sized by a count only the GPU knows.
//
// Null resolves the args from the buffer's real bytes, so the recorded
// workgroup counts are assertable with no GPU at all -- which is what makes
// this a shared case rather than a Metal-only one.
inline void CheckDispatchIndirectReadsItsCount(IRhiDevice& device) {
  ResetLog(device);
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);

  auto args = device.CreateBuffer(
      {.size = sizeof(DispatchIndirectArgs),
       .usage = BufferUsage::Indirect | BufferUsage::Storage |
                BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "dispatch_args"});
  REQUIRE(args);

  // Seeded on the CPU here; in the real pipeline a compute pass writes it.
  const DispatchIndirectArgs seeded{.x = 7, .y = 2, .z = 1};
  args->Write(0, {reinterpret_cast<const uint8_t*>(&seeded), sizeof(seeded)});

  auto encoder = device.CreateCommandEncoder("indirect_dispatch");
  encoder->Transition(args.get(), ResourceState::IndirectArg);
  auto* pass = encoder->BeginComputePass("cs");
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->DispatchIndirect(args.get(), 0);
  pass->End();
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (auto* log = badlands::rhi::null::GetCommandLog(device)) {
    const auto* rec = log->Find(
        badlands::rhi::null::RecordedCommand::Kind::DispatchIndirect);
    REQUIRE(rec != nullptr);
    CHECK(rec->dispatch[0] == 7);
    CHECK(rec->dispatch[1] == 2);
    CHECK(rec->dispatch[2] == 1);
  }
}

// Zero groups is LEGAL for an indirect dispatch and refused for a direct one.
// The counts live in GPU memory, so nothing at record time can see them, and an
// empty cull result produces exactly this every frame in a working program.
inline void CheckZeroIndirectDispatchIsAllowed(IRhiDevice& device) {
  ResetLog(device);
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);

  auto args = device.CreateBuffer(
      {.size = sizeof(DispatchIndirectArgs),
       // MapRead so this takes the SAME resolve path as its sibling case. With
       // a different usage set, a resolve that broke only for this one would
       // still record a command and still pass, because the counts were never
       // checked either.
       .usage = BufferUsage::Indirect | BufferUsage::Storage |
                BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "zero_args"});
  REQUIRE(args);
  // Seeded explicitly rather than default-constructed: y and z default to 0
  // because this struct mirrors what the GPU writes, so a `{}` here would be
  // asserting the defaults rather than zero GROUPS.
  const DispatchIndirectArgs zero{.x = 0, .y = 1, .z = 1};
  args->Write(0, {reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)});

  auto encoder = device.CreateCommandEncoder("zero_dispatch");
  encoder->Transition(args.get(), ResourceState::IndirectArg);
  auto* pass = encoder->BeginComputePass("cs");
  REQUIRE(pass != nullptr);
  pass->SetPipeline(pipe.get());
  pass->DispatchIndirect(args.get(), 0);  // must NOT be refused
  pass->End();
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  if (auto* log = badlands::rhi::null::GetCommandLog(device)) {
    const auto* rec = log->Find(
        badlands::rhi::null::RecordedCommand::Kind::DispatchIndirect);
    REQUIRE(rec != nullptr);
    CHECK(log->Count(badlands::rhi::null::RecordedCommand::Kind::
                         DispatchIndirect) == 1);
    // The COUNTS, not just the command. Null leaves these at their {0,0,0}
    // default whenever the resolve fails, so "one command was recorded" cannot
    // tell a successful zero-resolve from a failed one -- and the two mean
    // opposite things about whether the backend read the buffer at all.
    CHECK(rec->dispatch[0] == 0);
    CHECK(rec->dispatch[1] == 1);
    CHECK(rec->dispatch[2] == 1);
  }
}

// A backend refusing an indirect call with no argument buffer.
//
// Exercised on an UNVALIDATED device deliberately. The decorator refuses this
// first, so with validation on the call never reaches the backend and the case
// would be measuring the decorator instead of the thing it names -- and the
// decorator is not there in release builds, which is where a backend that
// silently records a (0,0,0) dispatch does its damage.
inline void CheckIndirectDispatchWithoutArgsIsRefused(IRhiDevice& device) {
  REQUIRE_FALSE(device.IsValidationEnabled());
  ResetLog(device);
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);

  const std::string log = CaptureLog([&] {
    auto encoder = device.CreateCommandEncoder("no_args");
    auto* pass = encoder->BeginComputePass("cs");
    pass->SetPipeline(pipe.get());
    pass->DispatchIndirect(nullptr, 0);
    pass->End();
    encoder->Finish();
    device.Submit(*encoder);
    device.WaitIdle();
  });
  INFO(log);
  CHECK(log.find("DispatchIndirect") != std::string::npos);
  CHECK(log.find("no argument buffer") != std::string::npos);

  // Rule 3: refused means it did not happen. Recording the command anyway is
  // what made Null the silent one of the two backends.
  if (auto* l = badlands::rhi::null::GetCommandLog(device)) {
    CHECK(l->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DispatchIndirect) == 0);
  }
}

// Recording work with no pipeline bound.
//
// Unvalidated, for the same reason as the case above: the decorator catches
// this first, and it is not there in release. Both backends must refuse and
// say so -- Metal used to drop the call with no diagnostic at all, and Null
// used to record it as though it had happened, which is the same defect from
// opposite ends.
inline void CheckDrawWithoutPipelineIsRefused(IRhiDevice& device) {
  REQUIRE_FALSE(device.IsValidationEnabled());
  ResetLog(device);
  auto target = device.CreateTexture(
      {.width = 8, .height = 8, .format = Format::RGBA8Unorm,
       .usage = TextureUsage::RenderTarget, .label = "nopipe"});
  REQUIRE(target);

  const std::string log = CaptureLog([&] {
    auto encoder = device.CreateCommandEncoder("nopipe");
    encoder->Transition(target.get(), ResourceState::RenderTarget);
    RenderPassDesc rp;
    rp.label = "rp";
    rp.color_attachments.push_back({.view = target->GetDefaultView()});
    auto* pass = encoder->BeginRenderPass(rp);
    if (pass) {
      pass->Draw(3);                            // no SetPipeline
      pass->DrawIndexed(3);                     // nor an index buffer
      pass->End();
    }
    encoder->Finish();
    device.Submit(*encoder);
    device.WaitIdle();
  });
  INFO(log);
  CHECK(log.find("Draw with no pipeline bound") != std::string::npos);
  CHECK(log.find("DrawIndexed with no pipeline bound") != std::string::npos);

  if (auto* l = badlands::rhi::null::GetCommandLog(device)) {
    CHECK(l->Count(badlands::rhi::null::RecordedCommand::Kind::Draw) == 0);
    CHECK(l->Count(badlands::rhi::null::RecordedCommand::Kind::DrawIndexed) == 0);
  }
}

// Blend states must line up one-to-one with the colour attachments, or the
// pipeline must not exist.
//
// Creation-time, so it holds in release builds too (rule 13), and shared so
// the backends cannot disagree about which pipelines are constructible. Null
// runs no shaders and has no opinion on blending -- it makes this check anyway,
// because "what can exist" is the contract, not "what draws correctly".
inline void CheckMismatchedBlendStateCountIsRefused(IRhiDevice& device) {
  auto module = device.CreateShaderModule(
      MinimalGraphicsSource(device.GetBackend()), ShaderReflection{},
      "blend_counts");
  REQUIRE(module);

  RenderPipelinePtr pipe;
  const std::string log = CaptureLog([&] {
    pipe = device.CreateRenderPipeline(
        {.vertex_shader = module.get(), .vertex_entry = "vs_main",
         .fragment_shader = module.get(), .fragment_entry = "fs_main",
         .color_formats = {Format::RGBA8Unorm},
         // Two states, one attachment.
         .blend_states = {AlphaBlend(), AlphaBlend()},
         .label = "too_many_blend_states"});
  });
  INFO(log);
  CHECK(pipe == nullptr);
  CHECK(log.find("too_many_blend_states") != std::string::npos);
  CHECK(log.find("2 blend state(s) for 1 colour attachment(s)") !=
        std::string::npos);
}

// Empty blend_states is the opaque default and must stay legal, since every
// existing call site relies on it.
inline void CheckOpaquePipelineNeedsNoBlendState(IRhiDevice& device) {
  auto module = device.CreateShaderModule(
      MinimalGraphicsSource(device.GetBackend()), ShaderReflection{},
      "blend_opaque");
  REQUIRE(module);
  const std::string log = CaptureLog([&] {
    auto pipe = device.CreateRenderPipeline(
        {.vertex_shader = module.get(), .vertex_entry = "vs_main",
         .fragment_shader = module.get(), .fragment_entry = "fs_main",
         .color_formats = {Format::RGBA8Unorm},
         .label = "opaque"});
    CHECK(pipe != nullptr);
    // The desc round-trips, so a caller can tell opaque from "blending I
    // forgot to read back".
    if (pipe) CHECK(pipe->GetDesc().blend_states.empty());
  });
  INFO(log);
  CHECK(log.empty());
}

// Indirect arguments that do not fit their buffer.
//
// Unvalidated, because this is a BACKEND precondition rather than validation:
// the GPU reads these bytes itself, so an offset past the end is a read of
// whatever follows the buffer, and the decorator that would catch it compiles
// out of a shipping build.
//
// Both backends, because this is precisely where they came apart: Null has to
// resolve the counts on the CPU so it was forced to check, while Metal encoded
// the call unchecked. The Null suite stayed green while Metal faulted.
inline void CheckOutOfRangeIndirectArgsAreRefused(IRhiDevice& device) {
  REQUIRE_FALSE(device.IsValidationEnabled());
  ResetLog(device);
  auto pipe = MakeTestPipeline(device);
  REQUIRE(pipe);

  // 20-byte draw args and 12-byte dispatch args; neither fits at offset 56 of a
  // 64-byte buffer.
  auto args = device.CreateBuffer(
      {.size = 64,
       .usage = BufferUsage::Indirect | BufferUsage::Storage |
                BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "short_args"});
  REQUIRE(args);

  const std::string log = CaptureLog([&] {
    auto encoder = device.CreateCommandEncoder("oob");
    auto* pass = encoder->BeginComputePass("cs");
    pass->SetPipeline(pipe.get());
    pass->DispatchIndirect(args.get(), 56);
    pass->End();
    encoder->Finish();
    device.Submit(*encoder);
    device.WaitIdle();
  });
  INFO(log);
  CHECK(log.find("short_args") != std::string::npos);
  CHECK(log.find("past the end") != std::string::npos);

  // Rule 3: refused means it did not happen.
  if (auto* l = badlands::rhi::null::GetCommandLog(device)) {
    CHECK(l->Count(badlands::rhi::null::RecordedCommand::Kind::
                       DispatchIndirect) == 0);
  }
}

// The optional capability query.
//
// Untested on both Null and the decorator until now, which is worse than the
// "tested on Null only" rule 9 already calls insufficient: a decorator that
// answered instead of forwarding would let a caller run a 64-bit-atomic shader
// against a backend that executes nothing, and nothing would say so.
inline void CheckFeatureQueryAnswers(IRhiDevice& device) {
  const bool answer = device.Supports(DeviceFeature::Atomic64MinMax);
  // Stable: a query that answers differently per call cannot be branched on.
  CHECK(device.Supports(DeviceFeature::Atomic64MinMax) == answer);

  // The decorator must FORWARD, not answer.
  if (IRhiDevice* inner = device.Inner()) {
    CHECK(answer == inner->Supports(DeviceFeature::Atomic64MinMax));
  }

  // Null runs no shaders, so it supports no shader-level feature. Asserted
  // rather than assumed -- a Null that started claiming otherwise would make
  // every capability-gated test pass against a backend that ran nothing.
  if (device.GetBackend() == BackendKind::Null) CHECK_FALSE(answer);

  // Every enumerator names itself, or a refusal cannot say which feature.
  CHECK(std::string(ToString(DeviceFeature::Atomic64MinMax)) ==
        "Atomic64MinMax");
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
                                   .dimension = TextureDimension::Tex2DArray,
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
                                   .dimension = TextureDimension::Tex2DArray,
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
                                   .dimension = TextureDimension::Tex2DArray,
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
  CheckFrameAllocatorSurvivesGrowthFailure(device);
  CheckWildBufferOffsetsAreRefused(device);
  CheckWaitIdleDoesNotRetireTheOpenFrame(device);
  CheckDynamicOffsetsReachTheBackend(device);
  CheckDispatchIndirectReadsItsCount(device);
  CheckZeroIndirectDispatchIsAllowed(device);
  CheckFeatureQueryAnswers(device);
  CheckOpaquePipelineNeedsNoBlendState(device);
  CheckSwapchainAcquirePresentCycle(device);
  CheckSwapchainSkipsWhenZeroSized(device);
  CheckSwapchainResize(device);
  CheckSwapchainReportsItsColorSpace(device);
  CheckUntaggedExtendedRangeSurfaceIsRefused(device);
  CheckResizeKeepsTheFormat(device);
  CheckTextureCreationAndViews(device);
  CheckCubeTexturesAndTheirViews(device);
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
