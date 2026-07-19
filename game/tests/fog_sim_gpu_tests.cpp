// Map fog GENERATOR GPU tests (Task: map fog generator). Headless harness that
// composes the fog media from world-static emitters (compute/fog_fill.wesl +
// shaders/common/fog_emitters.wesl, driven by FogSimulation's emitter/broadphase
// buffers) and reads back the media σ_t via the tests/media_slice_extract copy.
// Each emitter is verified independently against a CPU mirror of the evaluator.
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch_amalgamated.hpp>
#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/fog_cascade.hpp"
#include "engine/rendering/fog_sim.hpp"
#include "engine/rendering/fog_simulation.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
};

TestGpu& GetTestGpu() {
  static TestGpu* g = [] {
    auto* t = new TestGpu();
    wgpu::InstanceDescriptor idesc = {};
    t->instance = wgpu::CreateInstance(&idesc);
    REQUIRE(t->instance);
    wgpu::Adapter adapter = test::RequestAdapter(t->instance);
    REQUIRE(adapter);
    t->device = test::RequestDevice(adapter);
    REQUIRE(t->device);
    t->queue = t->device.GetQueue();
    t->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(t->device, FindShaderDirectory());
    return t;
  }();
  return *g;
}

wgpu::Buffer MakeUniform(wgpu::Device device, wgpu::Queue queue, const void* data,
                         size_t size) {
  wgpu::BufferDescriptor bd;
  bd.size = (size + 15u) & ~size_t{15u};
  bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer buf = device.CreateBuffer(&bd);
  queue.WriteBuffer(buf, 0, data, size);
  return buf;
}

// GPU mirror of shaders/compute/fog_fill.wesl FogFillParams (96 bytes).
struct FillParams {
  int32_t minVoxelX, minVoxelZ, boxOffX, boxOffZ;
  int32_t boxSizeX, boxSizeZ, resXZ, resY;
  int32_t cascadeIndex, cascadeCount, pad0, pad1;
  float voxelSizeXZ, voxelSizeY, floorY, height;
  float bpMinX, bpMinZ, bpCellSize;
  int32_t bpNx, bpNz;
  uint32_t emitterCount;
  int32_t useEmitters;
  float time;
};
static_assert(sizeof(FillParams) == 96);

struct SliceParams {
  int32_t slice, pad0, pad1, pad2;
};

// A 64^2 x 32 cascade covering world [-32,32]^2 at 1 m XZ voxels (2 m Y).
const fog::CascadeLayout kL{.cascade_count = 1, .res_xz = 64, .res_y = 32,
                            .base_half_extent = 32.0f, .floor_y = 0.0f,
                            .height = 64.0f};

// World position of media texel (tx,tz) at Z-slice sy — mirrors fog_fill.wesl's
// froxel→world reconstruction for a static camera at the origin.
glm::vec3 TexelWorld(int tx, int tz, int sy) {
  const int minVX = fog::CascadeMinVoxel(kL, 0, 0.0f);
  const int minVZ = fog::CascadeMinVoxel(kL, 0, 0.0f);
  const float vxz = fog::CascadeVoxelSizeXZ(kL, 0);
  const float vy = fog::CascadeVoxelSizeY(kL);
  const int wvx = minVX + fog::PosMod(tx - minVX, kL.res_xz);
  const int wvz = minVZ + fog::PosMod(tz - minVZ, kL.res_xz);
  return glm::vec3((static_cast<float>(wvx) + 0.5f) * vxz,
                   kL.floor_y + (static_cast<float>(sy) + 0.5f) * vy,
                   (static_cast<float>(wvz) + 0.5f) * vxz);
}

// CPU mirror of fog_emitters.wesl fogEmitterEval for a DISC emitter (fill == 1).
float DiscEval(const fog::Emitter& e, glm::vec3 world) {
  const glm::vec2 d = glm::vec2(world.x, world.z) - e.center;
  const float c = std::cos(e.rotation), s = std::sin(e.rotation);
  const glm::vec3 local(d.x * c + d.y * s, world.y - e.base_y,
                        -d.x * s + d.y * c);
  if (local.y < 0.0f || local.y > e.height) return 0.0f;
  float rnd;
  if (e.shape == fog::EmitterShape::Disc) {
    rnd = glm::length(glm::vec2(local.x, local.z)) / std::max(e.half_extent.x, 1e-4f);
  } else {
    rnd = std::max(std::abs(local.x) / std::max(e.half_extent.x, 1e-4f),
                   std::abs(local.z) / std::max(e.half_extent.y, 1e-4f));
  }
  if (rnd > 1.0f) return 0.0f;
  auto falloff = [](float nd, float f) {
    return std::clamp(1.0f - (nd - (1.0f - f)) / std::max(f, 1e-4f), 0.0f, 1.0f);
  };
  const float vnd = std::abs(local.y / std::max(e.height, 1e-4f) - 0.5f) * 2.0f;
  return e.magnitude * falloff(rnd, e.radial_falloff) *
         falloff(vnd, e.vertical_falloff);
}

// Compose the media from `emitters` at `time`, then copy Z-slice `slice`'s σ_t to
// an R32Float readback.
CpuImage ComposeMediaSlice(std::span<const fog::Emitter> emitters, float time,
                           int slice) {
  TestGpu& g = GetTestGpu();
  const uint32_t res = static_cast<uint32_t>(kL.res_xz);

  FogSimulation sim;
  sim.Initialize(g.device, g.queue);
  FogSimParams sp;
  sp.map_min = {-32.0f, -32.0f};
  sp.map_max = {32.0f, 32.0f};
  sp.bp_cell_size = 32.0f;
  sim.SetSources(emitters, sp);
  sim.AddTime(time);

  wgpu::TextureDescriptor md;
  md.dimension = wgpu::TextureDimension::e3D;
  md.size = {res, res, static_cast<uint32_t>(kL.res_y)};
  md.format = wgpu::TextureFormat::RGBA16Float;
  md.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
  md.mipLevelCount = 1;
  md.sampleCount = 1;
  wgpu::Texture media = g.device.CreateTexture(&md);
  wgpu::TextureView media_view = media.CreateView();

  wgpu::TextureDescriptor od;
  od.dimension = wgpu::TextureDimension::e2D;
  od.size = {res, res, 1};
  od.format = wgpu::TextureFormat::R32Float;
  od.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc;
  od.mipLevelCount = 1;
  od.sampleCount = 1;
  wgpu::Texture out = g.device.CreateTexture(&od);
  wgpu::TextureView out_view = out.CreateView();

  auto fill = g.pipeline_gen->GetComputePipeline("compute/fog_fill");
  auto ext = g.pipeline_gen->GetComputePipeline("tests/media_slice_extract");
  REQUIRE(fill);
  REQUIRE(fill->pipeline);
  REQUIRE(ext);
  REQUIRE(ext->pipeline);

  FillParams p{};
  p.minVoxelX = fog::CascadeMinVoxel(kL, 0, 0.0f);
  p.minVoxelZ = fog::CascadeMinVoxel(kL, 0, 0.0f);
  p.boxSizeX = static_cast<int>(res);
  p.boxSizeZ = static_cast<int>(res);
  p.resXZ = static_cast<int>(res);
  p.resY = kL.res_y;
  p.cascadeCount = 1;
  p.voxelSizeXZ = fog::CascadeVoxelSizeXZ(kL, 0);
  p.voxelSizeY = fog::CascadeVoxelSizeY(kL);
  p.floorY = kL.floor_y;
  p.height = kL.height;
  p.bpMinX = sim.BpMin().x;
  p.bpMinZ = sim.BpMin().y;
  p.bpCellSize = sim.BpCellSize();
  p.bpNx = sim.BpNx();
  p.bpNz = sim.BpNz();
  p.emitterCount = sim.EmitterCount();
  p.useEmitters = 1;
  p.time = sim.Time();
  wgpu::Buffer pbuf = MakeUniform(g.device, g.queue, &p, sizeof(p));

  SliceParams sp2{slice, 0, 0, 0};
  wgpu::Buffer spbuf = MakeUniform(g.device, g.queue, &sp2, sizeof(sp2));

  wgpu::CommandEncoder enc = g.device.CreateCommandEncoder();
  {  // compose media
    std::array<wgpu::BindGroupEntry, 5> e{};
    e[0].binding = 0;
    e[0].buffer = pbuf;
    e[0].size = sizeof(p);
    e[1].binding = 1;
    e[1].textureView = media_view;
    e[2].binding = 2;
    e[2].buffer = sim.EmitterBuffer();
    e[2].size = wgpu::kWholeSize;
    e[3].binding = 3;
    e[3].buffer = sim.BpCellsBuffer();
    e[3].size = wgpu::kWholeSize;
    e[4].binding = 4;
    e[4].buffer = sim.BpIndicesBuffer();
    e[4].size = wgpu::kWholeSize;
    wgpu::BindGroup bg = CreateComputeBindGroup(g.device, *fill, e);
    wgpu::ComputePassEncoder cp = enc.BeginComputePass();
    cp.SetPipeline(fill->pipeline);
    cp.SetBindGroup(0, bg, 0, nullptr);
    cp.DispatchWorkgroups((res + 3) / 4, (res + 3) / 4,
                          (static_cast<uint32_t>(kL.res_y) + 3) / 4);
    cp.End();
  }
  {  // extract slice
    std::array<wgpu::BindGroupEntry, 3> e{};
    e[0].binding = 0;
    e[0].buffer = spbuf;
    e[0].size = sizeof(sp2);
    e[1].binding = 1;
    e[1].textureView = media_view;
    e[2].binding = 2;
    e[2].textureView = out_view;
    wgpu::BindGroup bg = CreateComputeBindGroup(g.device, *ext, e);
    wgpu::ComputePassEncoder cp = enc.BeginComputePass();
    cp.SetPipeline(ext->pipeline);
    cp.SetBindGroup(0, bg, 0, nullptr);
    cp.DispatchWorkgroups((res + 7) / 8, (res + 7) / 8, 1);
    cp.End();
  }
  wgpu::CommandBuffer cb = enc.Finish();
  g.queue.Submit(1, &cb);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  return readback.ReadTextureSync(out, res, res, wgpu::TextureFormat::R32Float);
}

}  // namespace

TEST_CASE("fog emitter: disc composes exactly its radial×vertical envelope",
          "[fogsim][gpu]") {
  fog::Emitter e;
  e.center = {0.0f, 0.0f};
  e.half_extent = {12.0f, 12.0f};
  e.shape = fog::EmitterShape::Disc;
  e.type = fog::EmitterType::Disc;
  e.base_y = 0.0f;
  e.height = 40.0f;
  e.magnitude = 0.06f;  // dense, physical σ_t (written to media directly)
  e.radial_falloff = 0.4f;
  e.vertical_falloff = 0.5f;
  std::array<fog::Emitter, 1> ems{e};

  const int slice = 6;  // worldY = (6.5)*2 = 13, inside [0,40]
  CpuImage img = ComposeMediaSlice(ems, 0.0f, slice);

  float max_err = 0.0f, peak = 0.0f, far = 0.0f;
  for (int tz = 0; tz < kL.res_xz; ++tz) {
    for (int tx = 0; tx < kL.res_xz; ++tx) {
      const glm::vec3 w = TexelWorld(tx, tz, slice);
      const float ref = DiscEval(e, w);
      const float got = img.GetFloat(tx, tz);
      max_err = std::max(max_err, std::abs(got - ref));
      peak = std::max(peak, ref);
      if (glm::length(w - glm::vec3(e.center.x, w.y, e.center.y)) > 20.0f) {
        far = std::max(far, got);  // well outside the 12 m radius
      }
    }
  }
  spdlog::info("disc: peak={} max_err={} far={}", peak, max_err, far);
  REQUIRE(peak > 0.02f);        // envelope present, physical magnitude
  REQUIRE(max_err < 0.0015f);   // matches the CPU mirror (RGBA16F precision)
  REQUIRE(far < 1e-4f);         // 0 outside the footprint
}

TEST_CASE("fog emitter: physical sigma_t is written directly (no normalization)",
          "[fogsim][gpu]") {
  // A near-flat disc (no falloff) reads back its magnitude verbatim at centre.
  fog::Emitter e;
  e.center = {0.0f, 0.0f};
  e.half_extent = {16.0f, 16.0f};
  e.shape = fog::EmitterShape::Disc;
  e.type = fog::EmitterType::Disc;
  e.base_y = 0.0f;
  e.height = 60.0f;
  e.magnitude = 0.006f;  // moderate fog
  e.radial_falloff = 0.05f;
  e.vertical_falloff = 0.05f;
  std::array<fog::Emitter, 1> ems{e};
  const int slice = kL.res_y / 2;  // mid-height -> vertical envelope ~1
  CpuImage img = ComposeMediaSlice(ems, 0.0f, slice);
  const float centre = img.GetFloat(0, 0);  // world ~(0.5,0.5)
  spdlog::info("physical: centre={} (expect ~0.006)", centre);
  REQUIRE(centre == Catch::Approx(0.006f).margin(3e-4f));
}

TEST_CASE("fog emitters: overlapping discs accumulate (sum)", "[fogsim][gpu]") {
  fog::Emitter a;
  a.center = {-4.0f, 0.0f};
  a.half_extent = {14.0f, 14.0f};
  a.shape = fog::EmitterShape::Disc;
  a.type = fog::EmitterType::Disc;
  a.base_y = 0.0f;
  a.height = 40.0f;
  a.magnitude = 0.03f;
  a.radial_falloff = 0.6f;
  a.vertical_falloff = 0.5f;
  fog::Emitter b = a;
  b.center = {4.0f, 0.0f};
  b.magnitude = 0.04f;
  std::array<fog::Emitter, 2> ems{a, b};

  const int slice = 6;
  CpuImage img = ComposeMediaSlice(ems, 0.0f, slice);
  float max_err = 0.0f, peak = 0.0f;
  for (int tz = 0; tz < kL.res_xz; ++tz) {
    for (int tx = 0; tx < kL.res_xz; ++tx) {
      const glm::vec3 w = TexelWorld(tx, tz, slice);
      const float ref = DiscEval(a, w) + DiscEval(b, w);  // additive extinction
      max_err = std::max(max_err, std::abs(img.GetFloat(tx, tz) - ref));
      peak = std::max(peak, ref);
    }
  }
  spdlog::info("additive: peak={} max_err={}", peak, max_err);
  REQUIRE(peak > 0.05f);       // overlap sums above either alone
  REQUIRE(max_err < 0.002f);
}

TEST_CASE("fog emitter: noise is deterministic, animated, and bounded",
          "[fogsim][gpu]") {
  fog::Emitter n;
  n.center = {0.0f, 0.0f};
  n.half_extent = {14.0f, 14.0f};
  n.shape = fog::EmitterShape::Disc;
  n.type = fog::EmitterType::Noise;
  n.base_y = 0.0f;
  n.height = 40.0f;
  n.magnitude = 0.08f;
  n.radial_falloff = 0.5f;
  n.vertical_falloff = 0.5f;
  n.noise_freq = 0.2f;
  n.noise_contrast = 1.5f;
  n.scroll = {0.0f, 0.6f, 0.0f};  // vertical scroll
  n.seed = 5.0f;
  std::array<fog::Emitter, 1> ems{n};
  const int slice = 8;

  CpuImage a = ComposeMediaSlice(ems, 1.0f, slice);
  CpuImage b = ComposeMediaSlice(ems, 1.0f, slice);   // same time
  CpuImage c = ComposeMediaSlice(ems, 4.0f, slice);   // later time (scrolled)

  int det_mismatch = 0, animated = 0, out_of_bounds = 0;
  float peak = 0.0f;
  for (int tz = 0; tz < kL.res_xz; ++tz) {
    for (int tx = 0; tx < kL.res_xz; ++tx) {
      const float va = a.GetFloat(tx, tz), vb = b.GetFloat(tx, tz),
                  vc = c.GetFloat(tx, tz);
      peak = std::max(peak, va);
      if (va != vb) ++det_mismatch;
      if (std::abs(va - vc) > 1e-4f) ++animated;
      const glm::vec3 w = TexelWorld(tx, tz, slice);
      if (glm::length(glm::vec2(w.x, w.z) - n.center) > n.half_extent.x + 1.0f &&
          va > 1e-4f) {
        ++out_of_bounds;
      }
    }
  }
  spdlog::info("noise: peak={} det_mismatch={} animated={} oob={}", peak,
               det_mismatch, animated, out_of_bounds);
  REQUIRE(peak > 0.01f);         // noise fill produced fog
  REQUIRE(det_mismatch == 0);    // deterministic at a fixed time
  REQUIRE(animated > 50);        // the volume scrolls with time
  REQUIRE(out_of_bounds == 0);   // bounded to the footprint
}
