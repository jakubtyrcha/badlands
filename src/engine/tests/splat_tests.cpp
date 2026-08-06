// The Dreams-style splat chain, proven by reading pixels.
//
//   [1] evaluate   Slang  sample an analytic SDF on a fixed 32^3 grid, seed a
//                         point per surface cell
//   [2] finalize   Slang  turn the GPU-known count into dispatch arguments
//   [3] splat      MSL    pack depth above payload, atomic_max into a uint64
//                         visibility buffer
//   [4] resolve    Slang  unpack the payload into colour
//
// WHY PASS 3 IS NOT SLANG. Slang 2026.14.1 -- the newest release -- emits
// `atomic_fetch_max_explicit` for Metal, which is the 32-bit family and which
// MSL rejects for 64-bit types. The correct spelling is `atomic_max_explicit`
// on a `device atomic_ulong*`: returns void, device address space only. Both
// Slang spellings fail identically (InterlockedMax on a buffer element, and
// Atomic<uint64_t>::max), while the same source emits correct InterlockedMax
// for HLSL -- so the gap is Metal-specific, not Slang-wide. Splitting one
// shader is the agreed answer; Slang is here to avoid maintaining two of
// everything, not as an end in itself.
//
// WHY THE ASSERTIONS ARE ORDERED. With three suspects for a wrong pixel -- the
// GPU, the RHI, and the shader toolchain -- the cases run from "smallest thing
// that can be wrong" upward, so a failure implicates only what that level
// added.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

using namespace badlands;
using namespace badlands::rhi;

namespace {

constexpr uint32_t kGrid = 32;   // cells per axis
// One pixel per grid column, deliberately. At any finer resolution a regular
// grid projects onto every Nth pixel and leaves stripes of background between
// them -- including, on the first attempt here, the exact centre pixel the
// coverage assertion samples. Matching the two makes cell i land on pixel i,
// so "covered" means what it reads as.
constexpr uint32_t kScreen = kGrid;
constexpr uint32_t kCapacity = 1 << 16;
constexpr float kExtent = 20.0f;  // the grid spans [-kExtent, +kExtent] per axis

// Mirrors SplatParams in shaders/slang/splat/splat_common.slang. Every member
// is a float4 or float4x4 so host and device layouts cannot drift -- MSL pads
// float3 to 16 bytes, which has already cost this project once.
struct SplatParams {
  glm::mat4 view_proj{1.0f};
  glm::vec4 sphere_a{0.0f};
  glm::vec4 sphere_b{0.0f};
  glm::vec4 grid{0.0f};
  glm::vec4 config{0.0f};
};
static_assert(sizeof(SplatParams) == 64 + 16 * 4,
              "SplatParams must stay float4-aligned throughout");

struct Splat {
  glm::vec4 pos_source{0.0f};
};
static_assert(sizeof(Splat) == 16, "Splat must stay one float4");

// Pass 3. Screen-aligned point splats: one pixel each, which is the smallest
// thing that still exercises the atomic. A disc footprint changes only how many
// pixels each point touches.
constexpr const char* kSplatKernel = R"(
#include <metal_stdlib>
using namespace metal;

struct SplatParams {
  float4x4 view_proj;
  float4 sphere_a;
  float4 sphere_b;
  float4 grid;
  float4 config;
};

kernel void cs_splat(device atomic_ulong* visbuffer [[buffer(0)]],
                     device const float4* splats [[buffer(1)]],
                     device const uint* counter [[buffer(2)]],
                     constant SplatParams& params [[buffer(3)]],
                     uint tid [[thread_position_in_grid]]) {
  // counter[1] is the CLAMPED count finalize published. The dispatch is
  // rounded up to whole threadgroups, so the tail threads must do nothing.
  if (tid >= counter[1]) { return; }

  float4 clip = params.view_proj * float4(splats[tid].xyz, 1.0);
  if (clip.w <= 0.0) { return; }
  float3 ndc = clip.xyz / clip.w;
  if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0) { return; }

  uint w = uint(params.config.z);
  uint h = uint(params.config.w);
  uint px = uint((ndc.x * 0.5 + 0.5) * float(w));
  uint py = uint((1.0 - (ndc.y * 0.5 + 0.5)) * float(h));
  if (px >= w || py >= h) { return; }

  // Reversed-Z: nearer is LARGER, so one atomic_max resolves the depth test
  // and commits the payload indivisibly. A 32-bit depth atomic plus a separate
  // payload store leaves a window where another thread's payload lands against
  // this thread's depth.
  uint depth_bits = uint(saturate(ndc.z) * 4294967040.0);
  ulong packed = (ulong(depth_bits) << 32) | ulong(tid);
  atomic_max_explicit(&visbuffer[py * w + px], packed, memory_order_relaxed);
}
)";

ShaderReflection SplatKernelReflection() {
  ShaderReflection r;
  r.bindings.push_back({.group = 0, .slot = 0, .name = "visbuffer",
                        .kind = BindingKind::StorageBuffer,
                        .location = {.space = 0, .index = 0}});
  r.bindings.push_back({.group = 0, .slot = 1, .name = "splats",
                        .kind = BindingKind::ReadOnlyStorageBuffer,
                        .location = {.space = 0, .index = 1}});
  r.bindings.push_back({.group = 0, .slot = 2, .name = "counter",
                        .kind = BindingKind::ReadOnlyStorageBuffer,
                        .location = {.space = 0, .index = 2}});
  r.bindings.push_back({.group = 0, .slot = 3, .name = "params",
                        .kind = BindingKind::UniformBuffer,
                        .location = {.space = 0, .index = 3}});
  ReflectedEntryPoint ep;
  ep.name = "cs_splat";
  ep.stage = ShaderStage::Compute;
  ep.workgroup_size[0] = 64;
  r.entry_points.push_back(ep);
  return r;
}

template <typename T>
std::span<const uint8_t> Bytes(const T& v) {
  return {reinterpret_cast<const uint8_t*>(&v), sizeof(T)};
}

// The world the tests share. The sphere centre is deliberately offset by
// irrational-looking amounts so that no cell lands exactly on the |d| ==
// half_cell boundary -- which is what lets the CPU oracle assert an EXACT
// count rather than a tolerance.
SplatParams MakeParams(float radius_b = -1.0f, float b_z_offset = 0.0f) {
  const float cell = kExtent * 2.0f / float(kGrid);

  SplatParams p;
  p.grid = glm::vec4(-kExtent, -kExtent, -kExtent, cell);
  p.config = glm::vec4(float(kGrid), float(kCapacity), float(kScreen),
                       float(kScreen));
  p.sphere_a = glm::vec4(0.137f, 0.211f, 0.309f, 8.0f);
  p.sphere_b = glm::vec4(0.137f, 0.211f, 0.309f + b_z_offset, radius_b);

  // Orthographic down -Z. Ortho keeps the projected size of a sphere analytic:
  // a radius of r world units is r/kExtent of a half-screen, which is what
  // makes the coverage assertion a closed form rather than a fudge factor.
  //
  // near and far are passed SWAPPED, which is how reversed-Z is spelled with
  // GLM_FORCE_DEPTH_ZERO_TO_ONE: it maps the near plane to 1 and the far plane
  // to 0, so nearer is larger and one atomic_max is the depth test.
  const glm::mat4 proj =
      glm::ortho(-kExtent, kExtent, -kExtent, kExtent, 100.0f, 0.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, kExtent * 2.0f),
                                     glm::vec3(0), glm::vec3(0, 1, 0));
  p.view_proj = proj * view;
  return p;
}

// The oracle: the same grid, the same test, on the CPU.
uint32_t CpuSplatCount(const SplatParams& p) {
  const float cell = p.grid.w;
  const float half_cell = cell * 0.5f;
  uint32_t count = 0;
  for (uint32_t z = 0; z < kGrid; ++z) {
    for (uint32_t y = 0; y < kGrid; ++y) {
      for (uint32_t x = 0; x < kGrid; ++x) {
        const glm::vec3 pt = glm::vec3(p.grid) +
                             (glm::vec3(float(x), float(y), float(z)) + 0.5f) * cell;
        if (p.sphere_a.w > 0.0f &&
            std::abs(glm::length(pt - glm::vec3(p.sphere_a)) - p.sphere_a.w) <= half_cell) {
          ++count;
        }
        if (p.sphere_b.w > 0.0f &&
            std::abs(glm::length(pt - glm::vec3(p.sphere_b)) - p.sphere_b.w) <= half_cell) {
          ++count;
        }
      }
    }
  }
  return count;
}

// Everything the chain needs, built once per test.
struct Chain {
  std::unique_ptr<IRhiDevice> device;
  std::unique_ptr<slang::SlangCompiler> compiler;
  ComputePipelinePtr evaluate, finalize, splat;
  RenderPipelinePtr resolve;
  BufferPtr params_buf, splats_buf, counter_buf, args_buf, visbuffer;
  TexturePtr colour;
  BufferPtr readback;

  explicit operator bool() const { return device && resolve; }
};

Chain MakeChain() {
  Chain c;
  c.device = CreateDevice({.backend = BackendKind::Metal,
                           .enable_validation = true,
                           .label = "splat_tests"});
  if (!c.device) return c;
  // REQUIRED, not skipped: a Metal device below the recorded M2 floor is a
  // configuration error, and skipping would hide the one thing this suite is
  // for.
  REQUIRE(c.device->Supports(DeviceFeature::Atomic64MinMax));

  const std::vector<std::string> paths = {"shaders/slang/splat"};
  c.compiler = slang::CreateSlangCompiler(paths);
  REQUIRE(c.compiler);

  auto load = [&](const char* module, const char* entry) -> ShaderModulePtr {
    auto compiled = c.compiler->Get({.module = module, .entry = entry},
                                    slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;
    return c.device->CreateShaderModule(compiled->source, compiled->reflection,
                                        std::string(module) + "::" + entry);
  };

  auto eval_mod = load("evaluate", "cs_evaluate");
  auto fin_mod = load("finalize", "cs_finalize");
  auto res_vs = load("resolve", "vs_fullscreen");
  auto res_fs = load("resolve", "fs_resolve");
  REQUIRE(eval_mod);
  REQUIRE(fin_mod);
  REQUIRE(res_vs);
  REQUIRE(res_fs);

  auto splat_mod = c.device->CreateShaderModule(
      kSplatKernel, SplatKernelReflection(), "splat_msl");
  REQUIRE(splat_mod);

  c.evaluate = c.device->CreateComputePipeline(
      {.shader = eval_mod.get(), .entry = "cs_evaluate", .label = "evaluate"});
  c.finalize = c.device->CreateComputePipeline(
      {.shader = fin_mod.get(), .entry = "cs_finalize", .label = "finalize"});
  c.splat = c.device->CreateComputePipeline(
      {.shader = splat_mod.get(), .entry = "cs_splat", .label = "splat"});
  c.resolve = c.device->CreateRenderPipeline(
      {.vertex_shader = res_vs.get(), .vertex_entry = "vs_fullscreen",
       .fragment_shader = res_fs.get(), .fragment_entry = "fs_resolve",
       .color_formats = {Format::RGBA8Unorm},
       .cull_mode = CullMode::None, .label = "resolve"});
  REQUIRE(c.evaluate);
  REQUIRE(c.finalize);
  REQUIRE(c.splat);
  REQUIRE(c.resolve);

  c.params_buf = c.device->CreateBuffer(
      {.size = sizeof(SplatParams),
       .usage = BufferUsage::Uniform | BufferUsage::CopyDst, .label = "params"});
  c.splats_buf = c.device->CreateBuffer(
      {.size = kCapacity * sizeof(Splat),
       .usage = BufferUsage::Storage | BufferUsage::MapRead |
                BufferUsage::CopyDst, .label = "splats"});
  // [0] running count, [1] the clamped count finalize publishes.
  c.counter_buf = c.device->CreateBuffer(
      {.size = 2 * sizeof(uint32_t),
       .usage = BufferUsage::Storage | BufferUsage::MapRead |
                BufferUsage::CopyDst, .label = "counter"});
  c.args_buf = c.device->CreateBuffer(
      {.size = sizeof(DispatchIndirectArgs),
       .usage = BufferUsage::Storage | BufferUsage::Indirect |
                BufferUsage::MapRead | BufferUsage::CopyDst, .label = "args"});
  c.visbuffer = c.device->CreateBuffer(
      {.size = uint64_t(kScreen) * kScreen * sizeof(uint64_t),
       .usage = BufferUsage::Storage | BufferUsage::MapRead |
                BufferUsage::CopyDst, .label = "visbuffer"});
  c.colour = c.device->CreateTexture(
      {.width = kScreen, .height = kScreen, .format = Format::RGBA8Unorm,
       .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
       .label = "colour"});
  c.readback = c.device->CreateBuffer(
      {.size = uint64_t(kScreen) * kScreen * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "readback"});
  REQUIRE(c.params_buf);
  REQUIRE(c.visbuffer);
  REQUIRE(c.colour);
  return c;
}

struct Frame {
  uint32_t splat_count = 0;       // what the SDF produced, before clamping
  DispatchIndirectArgs args{};
  std::vector<uint8_t> pixels;    // RGBA8, kScreen * kScreen
  std::vector<uint64_t> visbuffer;  // packed (depth << 32) | splat index
  std::vector<Splat> splats;
};

// Runs all four passes and reads everything back.
Frame RunChain(Chain& c, const SplatParams& params) {
  c.params_buf->Write(0, Bytes(params));
  const uint32_t zeros[2] = {0, 0};
  c.counter_buf->Write(0, {reinterpret_cast<const uint8_t*>(zeros),
                           sizeof(zeros)});
  std::vector<uint8_t> clear(uint64_t(kScreen) * kScreen * sizeof(uint64_t), 0);
  c.visbuffer->Write(0, clear);

  auto eval_table = c.device->CreateBindingTable(
      {.compute_pipeline = c.evaluate.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = c.params_buf.get()},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = c.splats_buf.get()},
                   {.slot = 2, .kind = BindingKind::StorageBuffer,
                    .buffer = c.counter_buf.get()}},
       .label = "evaluate"});
  auto fin_table = c.device->CreateBindingTable(
      {.compute_pipeline = c.finalize.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = c.params_buf.get()},
                   {.slot = 1, .kind = BindingKind::StorageBuffer,
                    .buffer = c.counter_buf.get()},
                   {.slot = 2, .kind = BindingKind::StorageBuffer,
                    .buffer = c.args_buf.get()}},
       .label = "finalize"});
  auto splat_table = c.device->CreateBindingTable(
      {.compute_pipeline = c.splat.get(),
       .entries = {{.slot = 0, .kind = BindingKind::StorageBuffer,
                    .buffer = c.visbuffer.get()},
                   {.slot = 1, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = c.splats_buf.get()},
                   {.slot = 2, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = c.counter_buf.get()},
                   {.slot = 3, .kind = BindingKind::UniformBuffer,
                    .buffer = c.params_buf.get()}},
       .label = "splat"});
  auto res_table = c.device->CreateBindingTable(
      {.render_pipeline = c.resolve.get(),
       .entries = {{.slot = 0, .kind = BindingKind::UniformBuffer,
                    .buffer = c.params_buf.get()},
                   {.slot = 1, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = c.visbuffer.get()},
                   {.slot = 2, .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = c.splats_buf.get()}},
       .label = "resolve"});
  REQUIRE(eval_table);
  REQUIRE(fin_table);
  REQUIRE(splat_table);
  REQUIRE(res_table);

  c.device->BeginFrame();
  auto encoder = c.device->CreateCommandEncoder("splat_chain");

  encoder->Transition(c.params_buf.get(), ResourceState::ShaderRead);
  encoder->Transition(c.splats_buf.get(), ResourceState::ShaderWrite);
  encoder->Transition(c.counter_buf.get(), ResourceState::ShaderWrite);
  auto* p1 = encoder->BeginComputePass("evaluate");
  p1->SetPipeline(c.evaluate.get());
  p1->SetBindingTable(0, eval_table.get());
  p1->Dispatch(kGrid / 4, kGrid / 4, kGrid / 4);
  p1->End();

  encoder->Transition(c.args_buf.get(), ResourceState::ShaderWrite);
  auto* p2 = encoder->BeginComputePass("finalize");
  p2->SetPipeline(c.finalize.get());
  p2->SetBindingTable(0, fin_table.get());
  p2->Dispatch(1);
  p2->End();

  // The declaration a DX12 barrier will be emitted from; Metal auto-tracks it.
  encoder->Transition(c.args_buf.get(), ResourceState::IndirectArg);
  encoder->Transition(c.splats_buf.get(), ResourceState::ShaderRead);
  encoder->Transition(c.counter_buf.get(), ResourceState::ShaderRead);
  encoder->Transition(c.visbuffer.get(), ResourceState::ShaderWrite);
  auto* p3 = encoder->BeginComputePass("splat");
  p3->SetPipeline(c.splat.get());
  p3->SetBindingTable(0, splat_table.get());
  p3->DispatchIndirect(c.args_buf.get(), 0);
  p3->End();

  encoder->Transition(c.visbuffer.get(), ResourceState::ShaderRead);
  encoder->Transition(c.colour.get(), ResourceState::RenderTarget);
  RenderPassDesc rp;
  rp.label = "resolve";
  rp.color_attachments.push_back({.view = c.colour->GetDefaultView(),
                                  .load_op = LoadOp::Clear,
                                  .store_op = StoreOp::Store,
                                  .clear_color = {0, 0, 0, 1}});
  auto* p4 = encoder->BeginRenderPass(rp);
  REQUIRE(p4 != nullptr);
  p4->SetViewport(0, 0, float(kScreen), float(kScreen));
  p4->SetPipeline(c.resolve.get());
  p4->SetBindingTable(0, res_table.get());
  p4->Draw(3);
  p4->End();

  encoder->Transition(c.colour.get(), ResourceState::CopySrc);
  encoder->Transition(c.readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(c.colour.get(), 0, 0, c.readback.get(), 0);
  encoder->Finish();
  c.device->Submit(*encoder);
  c.device->EndFrame();
  c.device->WaitIdle();

  Frame f;
  uint32_t counts[2] = {0, 0};
  REQUIRE(c.counter_buf->Read(0, {reinterpret_cast<uint8_t*>(counts),
                                  sizeof(counts)}));
  f.splat_count = counts[0];
  REQUIRE(c.args_buf->Read(0, {reinterpret_cast<uint8_t*>(&f.args),
                               sizeof(f.args)}));
  f.pixels.resize(uint64_t(kScreen) * kScreen * 4, 0);
  REQUIRE(c.readback->Read(0, f.pixels));

  // The visibility buffer and the points themselves, so a test can ask which
  // splat won a pixel rather than only what colour came out. Colour alone
  // cannot distinguish "the maximum won" from "the last writer won".
  f.visbuffer.resize(uint64_t(kScreen) * kScreen, 0);
  REQUIRE(c.visbuffer->Read(
      0, {reinterpret_cast<uint8_t*>(f.visbuffer.data()),
          f.visbuffer.size() * sizeof(uint64_t)}));
  f.splats.resize(std::min(f.splat_count, kCapacity));
  if (!f.splats.empty()) {
    REQUIRE(c.splats_buf->Read(0, {reinterpret_cast<uint8_t*>(f.splats.data()),
                                   f.splats.size() * sizeof(Splat)}));
  }
  return f;
}

struct Rgba { uint8_t r, g, b, a; };
Rgba At(const Frame& f, uint32_t x, uint32_t y) {
  const size_t o = (size_t(y) * kScreen + x) * 4;
  return {f.pixels[o], f.pixels[o + 1], f.pixels[o + 2], f.pixels[o + 3]};
}
bool IsBackground(Rgba c) { return c.r < 16 && c.g < 16 && c.b < 16; }

}  // namespace

// --- 1. Layout, before anything depends on it -------------------------------

TEST_CASE("splat: the host and device agree on the parameter layout",
          "[splat]") {
  // The cheapest failure to diagnose, so it runs first. Slang and MSL both pad
  // float3 to 16 bytes; a struct that drifts here corrupts every later
  // assertion in a way that looks like bad geometry rather than bad layout.
  auto c = MakeChain();
  REQUIRE(c);

  // Every offset the shaders index by hand.
  CHECK(offsetof(SplatParams, sphere_a) == 64);
  CHECK(offsetof(SplatParams, sphere_b) == 80);
  CHECK(offsetof(SplatParams, grid) == 96);
  CHECK(offsetof(SplatParams, config) == 112);
  CHECK(sizeof(SplatParams) == 128);
  CHECK(sizeof(Splat) == 16);
}

// --- 2. The count, against a CPU oracle -------------------------------------

TEST_CASE("splat: the GPU seeds exactly the cells the CPU does", "[splat]") {
  // Exact, not approximate. The sphere centre is offset by irrational-looking
  // amounts so no cell sits on the |d| == half_cell boundary, which is what
  // makes an exact comparison safe across two float implementations.
  auto c = MakeChain();
  REQUIRE(c);
  const auto params = MakeParams();
  const auto f = RunChain(c, params);

  const uint32_t expected = CpuSplatCount(params);
  INFO("gpu=" << f.splat_count << " cpu=" << expected);
  CHECK(f.splat_count == expected);
  CHECK(f.splat_count > 0);

  // And the indirect args cover exactly that many splats.
  CHECK(f.args.x == (f.splat_count + 63) / 64);
  CHECK(f.args.y == 1);
  CHECK(f.args.z == 1);
}

// --- 3. Coverage ------------------------------------------------------------

TEST_CASE("splat: the sphere covers the disc it should", "[splat]") {
  auto c = MakeChain();
  REQUIRE(c);
  const auto params = MakeParams();
  const auto f = RunChain(c, params);

  CHECK_FALSE(IsBackground(At(f, kScreen / 2, kScreen / 2)));
  CHECK(IsBackground(At(f, 0, 0)));
  CHECK(IsBackground(At(f, kScreen - 1, 0)));
  CHECK(IsBackground(At(f, 0, kScreen - 1)));
  CHECK(IsBackground(At(f, kScreen - 1, kScreen - 1)));

  // Orthographic, so a sphere of radius r fills a disc of radius
  // r/kExtent of a half-screen. The band is not symmetric on purpose: the
  // shell is one cell thick, so silhouette cells sit up to half a cell OUTSIDE
  // the true radius and the covered area exceeds the ideal disc slightly.
  size_t covered = 0;
  for (uint32_t y = 0; y < kScreen; ++y) {
    for (uint32_t x = 0; x < kScreen; ++x) {
      if (!IsBackground(At(f, x, y))) ++covered;
    }
  }
  const double r_px = (params.sphere_a.w / kExtent) * (kScreen / 2.0);
  const double disc = 3.14159265 * r_px * r_px;
  INFO("covered=" << covered << " disc=" << disc);
  CHECK(double(covered) > disc * 0.85);
  CHECK(double(covered) < disc * 1.40);
}

// --- 4. Occlusion: the assertion the whole technique is for -----------------

TEST_CASE("splat: the nearer sphere wins every contested pixel", "[splat]") {
  // Two coaxial spheres of equal radius, one displaced along the view axis, so
  // their silhouettes coincide and EVERY covered pixel is contested. Sphere A
  // resolves red, B green, so a pixel's colour names its winner.
  //
  // Three renders, not one. "B is green" alone would also hold if B were the
  // only sphere producing splats there, or if the resolve simply preferred the
  // later source -- so the same pixels are rendered with A alone, with B
  // nearer, and with B FARTHER. Reversing the depth order must reverse the
  // colour; a test that cannot go red when occlusion is backwards is not
  // testing occlusion.
  auto c = MakeChain();
  REQUIRE(c);

  const auto a_only = RunChain(c, MakeParams());
  const auto b_near = RunChain(c, MakeParams(/*radius_b=*/8.0f,
                                             /*b_z_offset=*/+6.0f));
  const auto b_far = RunChain(c, MakeParams(/*radius_b=*/8.0f,
                                            /*b_z_offset=*/-6.0f));

  // Both spheres really do land on the same pixels: with two spheres the count
  // roughly doubles while the covered area does not grow.
  CHECK(b_near.splat_count > a_only.splat_count * 3 / 2);

  size_t contested = 0, b_won = 0, a_won_when_b_far = 0;
  for (uint32_t y = 0; y < kScreen; ++y) {
    for (uint32_t x = 0; x < kScreen; ++x) {
      // Contested == covered by A alone AND still covered with B present.
      if (IsBackground(At(a_only, x, y))) continue;
      if (IsBackground(At(b_near, x, y))) continue;
      ++contested;
      const Rgba near_px = At(b_near, x, y);
      const Rgba far_px = At(b_far, x, y);
      if (near_px.g > 200 && near_px.r < 64) ++b_won;
      if (far_px.r > 200 && far_px.g < 64) ++a_won_when_b_far;
    }
  }
  INFO("contested=" << contested << " b_won=" << b_won
                    << " a_won_when_b_far=" << a_won_when_b_far);
  CHECK(contested > 100);
  // EVERY one, not most: a single pixel where the farther payload survived is
  // the race this technique exists to close.
  CHECK(b_won == contested);
  CHECK(a_won_when_b_far == contested);
}

// --- 5. Contention on a single address --------------------------------------

TEST_CASE("splat: maximum contention still resolves to the nearest", "[splat]") {
  // Every splat onto one pixel: a degenerate ortho view that collapses the
  // whole sphere into a single texel. This is the worst serialisation the
  // technique can produce, and the atomic's contract must still hold.
  auto c = MakeChain();
  REQUIRE(c);
  auto params = MakeParams();
  // Collapse x and y so every splat lands on the centre texel; keep z varying
  // so their depths still differ and there is a real maximum to find.
  // Column-major: column 3 is the translation, so w stays 1 and x/y stay 0.
  params.view_proj = glm::mat4(0.0f);
  params.view_proj[3] = glm::vec4(0.0f, 0.0f, 0.5f, 1.0f);
  params.view_proj[2][2] = 1.0f / (kExtent * 2.0f);  // world z -> [0, 1]

  const auto f = RunChain(c, params);
  CHECK(f.splat_count > 0);
  // One pixel lit, the rest background: a single winner out of thousands.
  size_t covered = 0;
  for (uint32_t y = 0; y < kScreen; ++y) {
    for (uint32_t x = 0; x < kScreen; ++x) {
      if (!IsBackground(At(f, x, y))) ++covered;
    }
  }
  INFO("covered=" << covered << " of " << (kScreen * kScreen));
  CHECK(covered == 1);

  // ...and the winner is the NEAREST of them, not merely the last to arrive.
  // Counting lit pixels alone would pass just as well against a plain store,
  // so the payload is checked against a CPU maximum over the same points. The
  // grid steps z by a whole cell, far coarser than the depth quantisation, so
  // the maximum is unique and comparing world z is exact.
  const uint64_t packed = f.visbuffer[size_t(kScreen / 2) * kScreen + kScreen / 2];
  REQUIRE(packed != 0);
  const uint32_t winner = uint32_t(packed & 0xFFFFFFFFu);
  REQUIRE(winner < f.splats.size());

  float best_z = -std::numeric_limits<float>::infinity();
  for (const auto& s : f.splats) best_z = std::max(best_z, s.pos_source.z);
  INFO("winner z=" << f.splats[winner].pos_source.z << " best z=" << best_z
                   << " over " << f.splats.size() << " splats");
  CHECK(f.splats[winner].pos_source.z == best_z);
}

// --- 6. The empty case ------------------------------------------------------

TEST_CASE("splat: an empty SDF dispatches zero groups and renders nothing",
          "[splat]") {
  // A zero-group indirect dispatch is legal and must not be refused -- the
  // asymmetry with direct Dispatch, exercised end to end.
  auto c = MakeChain();
  REQUIRE(c);
  auto params = MakeParams();
  params.sphere_a.w = -1.0f;  // both spheres disabled
  const auto f = RunChain(c, params);

  CHECK(f.splat_count == 0);
  CHECK(f.args.x == 0);
  for (uint32_t y = 0; y < kScreen; y += 8) {
    for (uint32_t x = 0; x < kScreen; x += 8) {
      CHECK(IsBackground(At(f, x, y)));
    }
  }
}

// --- 7. Determinism ---------------------------------------------------------

TEST_CASE("splat: repeated runs are byte-identical", "[splat]") {
  // The property that proves the atomic is doing the work rather than the
  // schedule happening to be favourable. Thousands of racing threads, one
  // answer, every time.
  auto c = MakeChain();
  REQUIRE(c);
  const auto params = MakeParams(/*radius_b=*/8.0f, /*b_z_offset=*/6.0f);

  const auto first = RunChain(c, params);
  for (int i = 0; i < 4; ++i) {
    const auto again = RunChain(c, params);
    CHECK(again.splat_count == first.splat_count);
    CHECK(again.pixels == first.pixels);
  }
}
