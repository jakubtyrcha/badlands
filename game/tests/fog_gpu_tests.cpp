// Graphical fog tests (Task: fog rendering). Headless GPU harness that isolates
// fog INJECTION from fog RENDERING. The interface under test is the data in the
// cascades (the media 3D texture: rgb = sigma_s, a = sigma_t): each scene writes
// that media with an inlined test-fill shader (shaders/tests/fog_fill_test.wesl)
// and the PRODUCTION raymarch (shaders/passes/fog.wesl) + composite
// (shaders/passes/fog_composite.wesl) consume it. Each scene both ASSERTS pixel
// values and emits a reference PNG (build/fog_test_out/<scene>.png).
//
// Built on the sampo-ported headless GPU-test infra (gpu_test_helpers +
// ColorRenderTarget + TextureReadback), same as the shadow suite.
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <catch_amalgamated.hpp>
#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "engine/app/screenshot.hpp"  // WriteTextureToPng
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/fog_cascade.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 256;

// --- GPU-side uniform mirrors ---

// shaders/common/fog_types.wesl FogUniforms (64 bytes).
struct FogUniformsGpu {
  float fog_max_distance, phase_g, floor_y, height;
  float base_half_extent, ambient_scale, sun_scale, step_count;
  float res_y, cascade_count, enable_shafts, jitter_enabled;
  float fog_render_w, fog_render_h, frame_index, pad0;
};
static_assert(sizeof(FogUniformsGpu) == 64);

// shaders/tests/fog_fill_test.wesl FogFillParams (112 bytes).
struct FogFillParams {
  int32_t min_voxel_x, min_voxel_z, res_xz, res_y;
  int32_t cascade_index, cascade_count, mode, pad0;
  float voxel_size_xz, voxel_size_y, floor_y, height;
  float bx0, by0, bz0, sigma_t;
  float bx1, by1, bz1, grad_y0;
  float col0_r, col0_g, col0_b, grad_y1;
  float col1_r, col1_g, col1_b, pad1;
};
static_assert(sizeof(FogFillParams) == 112);

struct ExtractParams { float channel, pad0, pad1, pad2; };
struct CasterParams { glm::mat4 light_view_proj; glm::vec4 corners[4]; };
struct DepthQuadParams { glm::vec4 rect; float depth, pad0, pad1, pad2; };
struct StripeParams { float period, pad0, pad1, pad2; };

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

wgpu::Texture MakeColor(wgpu::Device device, uint32_t w, uint32_t h,
                        wgpu::TextureFormat fmt, wgpu::TextureUsage extra = wgpu::TextureUsage::None) {
  wgpu::TextureDescriptor d;
  d.size = {w, h, 1};
  d.format = fmt;
  d.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | extra;
  d.dimension = wgpu::TextureDimension::e2D;
  return device.CreateTexture(&d);
}

wgpu::Texture MakeDepth(wgpu::Device device, uint32_t w, uint32_t h) {
  wgpu::TextureDescriptor d;
  d.size = {w, h, 1};
  d.format = wgpu::TextureFormat::Depth32Float;
  d.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  d.dimension = wgpu::TextureDimension::e2D;
  return device.CreateTexture(&d);
}

void ClearDepth(wgpu::CommandEncoder& enc, wgpu::TextureView view, float value) {
  wgpu::RenderPassDepthStencilAttachment ds;
  ds.view = view;
  ds.depthClearValue = value;
  ds.depthLoadOp = wgpu::LoadOp::Clear;
  ds.depthStoreOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 0;
  rp.colorAttachments = nullptr;
  rp.depthStencilAttachment = &ds;
  enc.BeginRenderPass(&rp).End();
}

// --- Scene config ---

enum class FillMode { kUniform = 0, kGradient = 1 };
enum class Occluder { kNone, kQuad, kStripes };
enum class ExtractFrom { kFogTarget, kComposited };

struct FogScene {
  glm::vec3 cam_pos{0.0f, 32.0f, 0.0f};
  glm::vec3 cam_dir{1.0f, 0.0f, 0.0f};
  glm::vec3 cam_up{0.0f, 1.0f, 0.0f};
  fog::CascadeLayout layout{
      .cascade_count = 1, .res_xz = 128, .res_y = 64,
      .base_half_extent = 64.0f, .floor_y = 0.0f, .height = 64.0f};
  int step_count = 128;
  float fog_max_distance = 50.0f;
  float phase_g = 0.0f;
  bool half_res = false;
  bool shafts = false;

  // Fill (cascade-data interface).
  FillMode fill_mode = FillMode::kUniform;
  glm::vec3 box_min{10.0f, 0.0f, -40.0f};
  glm::vec3 box_max{30.0f, 64.0f, 40.0f};
  float sigma_t = 0.05f;
  float grad_y0 = 0.0f, grad_y1 = 10.0f;
  glm::vec3 grad_col0{1.0f, 0.0f, 0.0f};
  glm::vec3 grad_col1{0.0f, 0.0f, 1.0f};

  glm::vec3 sun_dir{0.0f, 1.0f, 0.0f};
  glm::vec3 sun_color{1.0f, 1.0f, 1.0f};

  // Camera-depth pillar (Monolith).
  bool pillar = false;
  glm::vec4 pillar_rect{-0.18f, -1.0f, 0.18f, 1.0f};  // NDC x0,y0,x1,y1
  float pillar_depth = 0.98f;                          // reversed-Z near

  // Shadow occluder.
  Occluder occluder = Occluder::kNone;
  glm::mat4 light_view_proj{1.0f};
  glm::vec4 caster_corners[4] = {};
  float stripe_period = 48.0f;

  // Output.
  ExtractFrom extract_from = ExtractFrom::kFogTarget;
  float extract_channel = 3.0f;  // 3=transmittance, 4=luminance, 0/1/2=rgb
  glm::vec3 bg_color{0.02f, 0.03f, 0.05f};  // composite background
  std::string png_name;  // if set, write build/fog_test_out/<name>.png
};

// Renders one fog frame per `cfg`; returns the R32Float readback of the chosen
// source/channel. Writes a beauty PNG (composite over bg, tonemapped) when
// cfg.png_name is set.
CpuImage RenderFogScene(const FogScene& cfg) {
  TestGpu& g = GetTestGpu();
  wgpu::Device device = g.device;
  wgpu::Queue queue = g.queue;
  GpuPipelineGenerator& gen = *g.pipeline_gen;
  const fog::CascadeLayout& L = cfg.layout;

  const uint32_t render_w = cfg.half_res ? kW / 2 : kW;
  const uint32_t render_h = cfg.half_res ? kH / 2 : kH;

  // Media cascade + sampler.
  wgpu::TextureDescriptor md;
  md.dimension = wgpu::TextureDimension::e3D;
  md.size = {static_cast<uint32_t>(L.res_xz), static_cast<uint32_t>(L.res_xz),
             static_cast<uint32_t>(L.res_y * L.cascade_count)};
  md.format = wgpu::TextureFormat::RGBA16Float;
  md.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
  wgpu::Texture media = device.CreateTexture(&md);
  wgpu::TextureView media_view = media.CreateView();
  wgpu::SamplerDescriptor sd;
  sd.addressModeU = wgpu::AddressMode::Repeat;
  sd.addressModeV = wgpu::AddressMode::Repeat;
  sd.addressModeW = wgpu::AddressMode::ClampToEdge;
  sd.magFilter = wgpu::FilterMode::Linear;
  sd.minFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler media_sampler = device.CreateSampler(&sd);

  // Frame UBO.
  Camera camera;
  camera.position = cfg.cam_pos;
  camera.direction = glm::normalize(cfg.cam_dir);
  camera.up = cfg.cam_up;
  camera.aspect = static_cast<float>(kW) / static_cast<float>(kH);
  UniformData fu{};
  fu.view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  fu.proj = camera.GetProj();
  fu.view_prev = fu.view;
  fu.proj_prev = fu.proj;
  fu.light_view_proj = cfg.light_view_proj;
  fu.camera_world_pos = glm::vec4(cfg.cam_pos, 0.0f);
  fu.sunDir = glm::vec4(glm::normalize(cfg.sun_dir), 0.0f);
  fu.sunColor = glm::vec4(cfg.sun_color, 0.0f);
  for (int i = 0; i < 9; ++i) fu.ambient_sh[i] = glm::vec4(0.0f);  // no ambient
  fu.near_plane = camera.near_plane;
  fu.far_plane = camera.far_plane;
  fu.screen_size = glm::vec2(kW, kH);
  fu.output_is_linear = 0;  // RGBA8 tonemap target -> apply sRGB
  wgpu::Buffer frame_ubo = MakeUniform(device, queue, &fu, sizeof(fu));

  // Fog UBO.
  FogUniformsGpu fog{};
  fog.fog_max_distance = cfg.fog_max_distance;
  fog.phase_g = cfg.phase_g;
  fog.floor_y = L.floor_y;
  fog.height = L.height;
  fog.base_half_extent = L.base_half_extent;
  fog.ambient_scale = 1.0f;
  fog.sun_scale = 1.0f;
  fog.step_count = static_cast<float>(cfg.step_count);
  fog.res_y = static_cast<float>(L.res_y);
  fog.cascade_count = static_cast<float>(L.cascade_count);
  fog.enable_shafts = cfg.shafts ? 1.0f : 0.0f;
  fog.jitter_enabled = 0.0f;
  fog.fog_render_w = static_cast<float>(render_w);
  fog.fog_render_h = static_cast<float>(render_h);
  fog.frame_index = 0.0f;
  wgpu::Buffer fog_ubo = MakeUniform(device, queue, &fog, sizeof(fog));

  wgpu::Texture depth_tex = MakeDepth(device, kW, kH);
  wgpu::TextureView depth_view = depth_tex.CreateView();
  wgpu::Texture shadow_tex = MakeDepth(device, 512, 512);
  wgpu::TextureView shadow_view = shadow_tex.CreateView();
  wgpu::SamplerDescriptor cmp;
  cmp.compare = wgpu::CompareFunction::LessEqual;
  wgpu::Sampler shadow_sampler = device.CreateSampler(&cmp);

  wgpu::Texture fog_target = MakeColor(device, render_w, render_h, wgpu::TextureFormat::RGBA16Float);
  wgpu::TextureView fog_target_view = fog_target.CreateView();

  // Pipelines.
  auto fill_pipe = gen.GetComputePipeline("tests/fog_fill_test");
  auto ray_pipe = [&] {
    RenderPipelineDeclaration d; d.shader_path = "passes/fog";
    return gen.GetPipeline(d, {wgpu::TextureFormat::RGBA16Float});
  }();
  auto ext_pipe = [&] {
    RenderPipelineDeclaration d; d.shader_path = "tests/fog_extract";
    return gen.GetPipeline(d, {wgpu::TextureFormat::R32Float});
  }();
  REQUIRE(fill_pipe); REQUIRE(fill_pipe->pipeline);
  REQUIRE(ray_pipe); REQUIRE(ray_pipe->pipeline);
  REQUIRE(ext_pipe); REQUIRE(ext_pipe->pipeline);

  wgpu::CommandEncoder enc = device.CreateCommandEncoder();
  ClearDepth(enc, depth_view, 0.0f);   // sky (reversed-Z far)
  ClearDepth(enc, shadow_view, 1.0f);  // conventional-Z far -> all lit

  // Optional camera-depth pillar.
  if (cfg.pillar) {
    RenderPipelineDeclaration d;
    d.shader_path = "tests/fog_depth_quad";
    d.depth_write = true;
    d.depth_compare = wgpu::CompareFunction::GreaterEqual;  // reversed-Z: nearer wins
    d.depth_format = wgpu::TextureFormat::Depth32Float;
    auto pipe = gen.GetPipeline(d, {wgpu::TextureFormat::R8Unorm});
    REQUIRE(pipe); REQUIRE(pipe->pipeline);
    DepthQuadParams dp{cfg.pillar_rect, cfg.pillar_depth, 0, 0, 0};
    wgpu::Buffer b = MakeUniform(device, queue, &dp, sizeof(dp));
    std::array<wgpu::BindGroupEntry, 1> e{};
    e[0].binding = 0; e[0].buffer = b; e[0].size = sizeof(dp);
    wgpu::BindGroup bg = CreateBindGroup(device, *pipe, 0, e);
    wgpu::Texture dummy = MakeColor(device, kW, kH, wgpu::TextureFormat::R8Unorm);
    wgpu::RenderPassColorAttachment ca; ca.view = dummy.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDepthStencilAttachment ds; ds.view = depth_view;
    ds.depthLoadOp = wgpu::LoadOp::Load; ds.depthStoreOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &ds;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(pipe->pipeline); pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(6); pass.End();
  }

  // Optional shadow occluder (quad or stripes).
  if (cfg.occluder != Occluder::kNone) {
    const bool stripes = cfg.occluder == Occluder::kStripes;
    RenderPipelineDeclaration d;
    d.shader_path = stripes ? "tests/fog_caster_stripes" : "tests/fog_caster";
    d.depth_write = true;
    d.depth_compare = wgpu::CompareFunction::Less;  // conventional-Z: nearer wins
    d.depth_format = wgpu::TextureFormat::Depth32Float;
    auto pipe = gen.GetPipeline(d, {wgpu::TextureFormat::R8Unorm});
    REQUIRE(pipe); REQUIRE(pipe->pipeline);
    wgpu::Buffer b;
    if (stripes) {
      StripeParams sp{cfg.stripe_period, 0, 0, 0};
      b = MakeUniform(device, queue, &sp, sizeof(sp));
    } else {
      CasterParams cp{}; cp.light_view_proj = cfg.light_view_proj;
      for (int i = 0; i < 4; ++i) cp.corners[i] = cfg.caster_corners[i];
      b = MakeUniform(device, queue, &cp, sizeof(cp));
    }
    std::array<wgpu::BindGroupEntry, 1> e{};
    e[0].binding = 0; e[0].buffer = b;
    e[0].size = stripes ? sizeof(StripeParams) : sizeof(CasterParams);
    wgpu::BindGroup bg = CreateBindGroup(device, *pipe, 0, e);
    wgpu::Texture dummy = MakeColor(device, 512, 512, wgpu::TextureFormat::R8Unorm);
    wgpu::RenderPassColorAttachment ca; ca.view = dummy.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDepthStencilAttachment ds; ds.view = shadow_view;
    ds.depthLoadOp = wgpu::LoadOp::Load; ds.depthStoreOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &ds;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(pipe->pipeline); pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(stripes ? 3u : 6u); pass.End();
  }

  // Fill each cascade (full grid; static camera).
  {
    wgpu::ComputePassEncoder cp = enc.BeginComputePass();
    cp.SetPipeline(fill_pipe->pipeline);
    for (int i = 0; i < L.cascade_count; ++i) {
      FogFillParams p{};
      p.min_voxel_x = fog::CascadeMinVoxel(L, i, cfg.cam_pos.x);
      p.min_voxel_z = fog::CascadeMinVoxel(L, i, cfg.cam_pos.z);
      p.res_xz = L.res_xz; p.res_y = L.res_y;
      p.cascade_index = i; p.cascade_count = L.cascade_count;
      p.mode = static_cast<int32_t>(cfg.fill_mode);
      p.voxel_size_xz = fog::CascadeVoxelSizeXZ(L, i);
      p.voxel_size_y = fog::CascadeVoxelSizeY(L);
      p.floor_y = L.floor_y; p.height = L.height;
      p.bx0 = cfg.box_min.x; p.by0 = cfg.box_min.y; p.bz0 = cfg.box_min.z;
      p.bx1 = cfg.box_max.x; p.by1 = cfg.box_max.y; p.bz1 = cfg.box_max.z;
      p.sigma_t = cfg.sigma_t;
      p.grad_y0 = cfg.grad_y0; p.grad_y1 = cfg.grad_y1;
      p.col0_r = cfg.grad_col0.r; p.col0_g = cfg.grad_col0.g; p.col0_b = cfg.grad_col0.b;
      p.col1_r = cfg.grad_col1.r; p.col1_g = cfg.grad_col1.g; p.col1_b = cfg.grad_col1.b;
      wgpu::Buffer pbuf = MakeUniform(device, queue, &p, sizeof(p));
      std::array<wgpu::BindGroupEntry, 2> e{};
      e[0].binding = 0; e[0].buffer = pbuf; e[0].size = sizeof(p);
      e[1].binding = 1; e[1].textureView = media_view;
      wgpu::BindGroup bg = CreateComputeBindGroup(device, *fill_pipe, e);
      cp.SetBindGroup(0, bg, 0, nullptr);
      const uint32_t dxy = (static_cast<uint32_t>(L.res_xz) + 3) / 4;
      const uint32_t dz = (static_cast<uint32_t>(L.res_y) + 3) / 4;
      cp.DispatchWorkgroups(dxy, dxy, dz);
    }
    cp.End();
  }

  const uint32_t fog_dyn = 0;

  // Production raymarch -> fog target.
  {
    std::array<wgpu::BindGroupEntry, 1> g0{};
    g0[0].binding = 0; g0[0].buffer = frame_ubo; g0[0].size = sizeof(fu);
    wgpu::BindGroup bg0 = CreateBindGroup(device, *ray_pipe, 0, g0);
    std::array<wgpu::BindGroupEntry, 6> g1{};
    g1[0].binding = 0; g1[0].buffer = fog_ubo; g1[0].size = sizeof(fog);
    g1[1].binding = 1; g1[1].textureView = media_view;
    g1[2].binding = 2; g1[2].sampler = media_sampler;
    g1[3].binding = 3; g1[3].textureView = depth_view;
    g1[4].binding = 4; g1[4].textureView = shadow_view;
    g1[5].binding = 5; g1[5].sampler = shadow_sampler;
    wgpu::BindGroup bg1 = CreateBindGroup(device, *ray_pipe, 1, g1);
    wgpu::RenderPassColorAttachment ca; ca.view = fog_target_view;
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(ray_pipe->pipeline);
    pass.SetBindGroup(0, bg0, 0, nullptr);
    pass.SetBindGroup(1, bg1, 1, &fog_dyn);
    pass.Draw(3); pass.End();
  }

  // Composite over background (full res) when needed (Composited extract / PNG).
  const bool need_composite =
      cfg.extract_from == ExtractFrom::kComposited || !cfg.png_name.empty();
  wgpu::Texture hdr = MakeColor(device, kW, kH, wgpu::TextureFormat::RGBA16Float);
  wgpu::TextureView hdr_view = hdr.CreateView();
  if (need_composite) {
    RenderPipelineDeclaration d; d.shader_path = "passes/fog_composite";
    wgpu::BlendState blend;
    blend.color.operation = wgpu::BlendOperation::Add;
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;
    blend.alpha.operation = wgpu::BlendOperation::Add;
    blend.alpha.srcFactor = wgpu::BlendFactor::Zero;
    blend.alpha.dstFactor = wgpu::BlendFactor::One;
    d.custom_blend = blend;
    auto pipe = gen.GetPipeline(d, {wgpu::TextureFormat::RGBA16Float});
    REQUIRE(pipe); REQUIRE(pipe->pipeline);
    std::array<wgpu::BindGroupEntry, 1> g0{};
    g0[0].binding = 0; g0[0].buffer = frame_ubo; g0[0].size = sizeof(fu);
    wgpu::BindGroup bg0 = CreateBindGroup(device, *pipe, 0, g0);
    std::array<wgpu::BindGroupEntry, 3> g1{};
    g1[0].binding = 0; g1[0].buffer = fog_ubo; g1[0].size = sizeof(fog);
    g1[1].binding = 1; g1[1].textureView = fog_target_view;
    g1[2].binding = 2; g1[2].textureView = depth_view;
    wgpu::BindGroup bg1 = CreateBindGroup(device, *pipe, 1, g1);
    wgpu::RenderPassColorAttachment ca; ca.view = hdr_view;
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {cfg.bg_color.r, cfg.bg_color.g, cfg.bg_color.b, 1.0};
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(pipe->pipeline);
    pass.SetBindGroup(0, bg0, 0, nullptr);
    pass.SetBindGroup(1, bg1, 1, &fog_dyn);
    pass.Draw(3); pass.End();
  }

  // Extract chosen source/channel -> R32Float (for assertions).
  wgpu::TextureView src_view =
      cfg.extract_from == ExtractFrom::kComposited ? hdr_view : fog_target_view;
  const uint32_t src_w = cfg.extract_from == ExtractFrom::kComposited ? kW : render_w;
  const uint32_t src_h = cfg.extract_from == ExtractFrom::kComposited ? kH : render_h;
  ColorRenderTarget out_rt(device, src_w, src_h, wgpu::TextureFormat::R32Float);
  {
    RenderPipelineDeclaration d; d.shader_path = "tests/fog_extract";
    auto pipe = gen.GetPipeline(d, {wgpu::TextureFormat::R32Float});
    ExtractParams ep{cfg.extract_channel, 0, 0, 0};
    wgpu::Buffer b = MakeUniform(device, queue, &ep, sizeof(ep));
    std::array<wgpu::BindGroupEntry, 2> e{};
    e[0].binding = 0; e[0].buffer = b; e[0].size = sizeof(ep);
    e[1].binding = 1; e[1].textureView = src_view;
    wgpu::BindGroup bg = CreateBindGroup(device, *pipe, 0, e);
    wgpu::RenderPassColorAttachment ca; ca.view = out_rt.GetView();
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(pipe->pipeline); pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(3); pass.End();
  }

  // Beauty PNG: tonemap the composited HDR -> RGBA8.
  wgpu::Texture beauty;
  if (!cfg.png_name.empty()) {
    beauty = MakeColor(device, kW, kH, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::CopySrc);
    RenderPipelineDeclaration d; d.shader_path = "passes/tonemapping";
    auto pipe = gen.GetPipeline(d, {wgpu::TextureFormat::RGBA8Unorm});
    REQUIRE(pipe); REQUIRE(pipe->pipeline);
    // The resolve composites a UI overlay at @2 — bind a 1x1 transparent
    // texel (no UI in this test; a == 0 leaves the scene untouched).
    wgpu::TextureDescriptor od; od.size = {1, 1, 1};
    od.format = wgpu::TextureFormat::RGBA8Unorm;
    od.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    wgpu::Texture overlay = device.CreateTexture(&od);
    const uint32_t transparent = 0;
    wgpu::TexelCopyTextureInfo od_dst{}; od_dst.texture = overlay;
    wgpu::TexelCopyBufferLayout od_layout; od_layout.bytesPerRow = 4; od_layout.rowsPerImage = 1;
    wgpu::Extent3D od_ext = {1, 1, 1};
    queue.WriteTexture(&od_dst, &transparent, sizeof(transparent), &od_layout, &od_ext);
    std::array<wgpu::BindGroupEntry, 3> e{};
    e[0].binding = 0; e[0].buffer = frame_ubo; e[0].size = sizeof(fu);
    e[1].binding = 1; e[1].textureView = hdr_view;
    e[2].binding = 2; e[2].textureView = overlay.CreateView();
    wgpu::BindGroup bg = CreateBindGroup(device, *pipe, 0, e);
    wgpu::RenderPassColorAttachment ca; ca.view = beauty.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    pass.SetPipeline(pipe->pipeline); pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(3); pass.End();
  }

  wgpu::CommandBuffer cb = enc.Finish();
  queue.Submit(1, &cb);
  test::WaitForGpu(g.instance, device, queue);

  if (!cfg.png_name.empty()) {
    std::error_code code;
    std::filesystem::create_directories("build/fog_test_out", code);
    WriteTextureToPng(g.instance, device, queue, beauty, kW, kH,
                      "build/fog_test_out/" + cfg.png_name + ".png");
  }

  TextureReadback readback(g.instance, device, queue);
  return readback.ReadTextureSync(out_rt.GetTexture(), src_w, src_h,
                                  wgpu::TextureFormat::R32Float);
}

}  // namespace

// ===========================================================================
// Accumulation (Beer-Lambert) — existing tests, migrated to FogScene.
// ===========================================================================

TEST_CASE("fog: empty volume is fully transmissive", "[fog][gpu]") {
  FogScene cfg; cfg.sigma_t = 0.0f;
  CpuImage img = RenderFogScene(cfg);
  REQUIRE(img.GetFloat(kW / 2, kH / 2) == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("fog: transmittance through a slab matches Beer-Lambert", "[fog][gpu]") {
  FogScene cfg; cfg.sigma_t = 0.05f;  // 20 m slab -> exp(-1)
  const float t = RenderFogScene(cfg).GetFloat(kW / 2, kH / 2);
  REQUIRE(t == Catch::Approx(std::exp(-1.0f)).margin(0.04f));
}

TEST_CASE("fog: doubling extinction squares the transmittance", "[fog][gpu]") {
  FogScene a; a.sigma_t = 0.05f;
  FogScene b = a; b.sigma_t = 0.10f;
  const float ta = RenderFogScene(a).GetFloat(kW / 2, kH / 2);
  const float tb = RenderFogScene(b).GetFloat(kW / 2, kH / 2);
  REQUIRE(tb == Catch::Approx(ta * ta).margin(0.02f));
}

// Ortho light straight down (shared by shadow/venetian scenes): world y in
// [0,64] -> conventional-Z depth [1,0].
glm::mat4 DownLightViewProj() {
  glm::mat4 lp = glm::ortho(-64.0f, 64.0f, -64.0f, 64.0f, 136.0f, 200.0f);
  glm::mat4 lv = glm::lookAt(glm::vec3(0.0f, 200.0f, 0.0f), glm::vec3(0.0f),
                             glm::vec3(0.0f, 0.0f, 1.0f));
  return lp * lv;
}

// ===========================================================================
// Scene 1 — Infinite Ground: toroidal world-lock + cascade transition.
// ===========================================================================
TEST_CASE("fog scene: infinite ground is world-locked under camera motion", "[fog][gpu]") {
  // A world-fixed thin slab; camera swept along +X across voxel and cascade
  // boundaries. Transmittance to the slab must stay constant (no popping).
  FogScene base;
  base.layout = fog::CascadeLayout{.cascade_count = 3, .res_xz = 128, .res_y = 64,
                                   .base_half_extent = 64.0f, .floor_y = 0.0f, .height = 64.0f};
  base.cam_dir = glm::vec3(1.0f, 0.0f, 0.0f);
  base.cam_up = glm::vec3(0.0f, 1.0f, 0.0f);
  base.box_min = glm::vec3(68.0f, 0.0f, -64.0f);   // slab at world x in [68,72]
  base.box_max = glm::vec3(72.0f, 64.0f, 64.0f);
  base.sigma_t = 0.1f;                             // T ~ exp(-0.4) = 0.67
  base.fog_max_distance = 90.0f;
  base.step_count = 192;

  const float xs[] = {0.0f, 1.3f, 2.7f, 4.1f, 6.5f, 9.0f};
  float tmin = 1e9f, tmax = -1e9f;
  for (float x : xs) {
    FogScene s = base;
    s.cam_pos = glm::vec3(x, 32.0f, 0.0f);
    const float t = RenderFogScene(s).GetFloat(kW / 2, kH / 2);
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  spdlog::info("infinite ground: T range [{}, {}]", tmin, tmax);
  REQUIRE(tmax - tmin < 0.05f);   // world-locked: no popping across the sweep
  REQUIRE(tmin > 0.4f);           // slab actually present (not empty)
  REQUIRE(tmax < 0.9f);
}

// A single quad caster occludes the sun over world x>0 -> in-scatter gap there.
TEST_CASE("fog scene: a single caster punches a shadow gap", "[fog][gpu]") {
  FogScene s;
  s.cam_pos = glm::vec3(0.0f, 80.0f, 0.0f);
  s.cam_dir = glm::vec3(0.0f, -1.0f, 0.0f);
  s.cam_up = glm::vec3(0.0f, 0.0f, -1.0f);
  s.sun_dir = glm::vec3(0.0f, 1.0f, 0.0f);
  s.box_min = glm::vec3(-64.0f, 0.0f, -64.0f);
  s.box_max = glm::vec3(64.0f, 64.0f, 64.0f);
  s.sigma_t = 0.03f;
  s.fog_max_distance = 100.0f;
  s.shafts = true;
  s.occluder = Occluder::kQuad;
  s.light_view_proj = DownLightViewProj();
  s.caster_corners[0] = glm::vec4(0.0f, 64.0f, -64.0f, 1.0f);
  s.caster_corners[1] = glm::vec4(64.0f, 64.0f, -64.0f, 1.0f);
  s.caster_corners[2] = glm::vec4(0.0f, 64.0f, 64.0f, 1.0f);
  s.caster_corners[3] = glm::vec4(64.0f, 64.0f, 64.0f, 1.0f);
  s.extract_channel = 4.0f;
  CpuImage img = RenderFogScene(s);
  const float lit = img.GetFloat(kW / 4, kH / 2);           // world x<0 -> lit
  const float shadowed = img.GetFloat(3 * kW / 4, kH / 2);  // world x>0 -> shadowed
  REQUIRE(lit > 1e-4f);
  REQUIRE(shadowed < lit * 0.25f);
}

// ===========================================================================
// Scene 2 — Venetian Blinds: parallel shafts through slit occluders.
// ===========================================================================
TEST_CASE("fog scene: venetian blinds produce alternating light shafts", "[fog][gpu]") {
  FogScene s;
  s.cam_pos = glm::vec3(0.0f, 80.0f, 0.0f);
  s.cam_dir = glm::vec3(0.0f, -1.0f, 0.0f);
  s.cam_up = glm::vec3(0.0f, 0.0f, -1.0f);
  s.sun_dir = glm::vec3(0.0f, 1.0f, 0.0f);
  s.box_min = glm::vec3(-64.0f, 0.0f, -64.0f);
  s.box_max = glm::vec3(64.0f, 64.0f, 64.0f);
  s.sigma_t = 0.03f;
  s.fog_max_distance = 100.0f;
  s.shafts = true;
  s.occluder = Occluder::kStripes;
  s.light_view_proj = DownLightViewProj();
  s.stripe_period = 24.0f;   // finer -> several shafts across the visible band
  s.extract_channel = 4.0f;  // in-scatter luminance
  s.png_name = "venetian";
  CpuImage img = RenderFogScene(s);

  // Scan a horizontal line; count mean crossings (alternating lit/dark).
  const int y = kH / 2;
  float sum = 0.0f;
  float vals[24];
  for (int i = 0; i < 24; ++i) {
    const int x = static_cast<int>((0.15f + 0.7f * i / 23.0f) * kW);
    vals[i] = img.GetFloat(x, y);
    sum += vals[i];
  }
  const float mean = sum / 24.0f;
  int crossings = 0;
  for (int i = 1; i < 24; ++i) {
    if ((vals[i - 1] < mean) != (vals[i] < mean)) ++crossings;
  }
  spdlog::info("venetian: mean={} crossings={}", mean, crossings);
  REQUIRE(mean > 1e-4f);   // fog is lit somewhere
  REQUIRE(crossings >= 4);  // several distinct shafts
}

// ===========================================================================
// Scene 3 — Monolith: depth-aware upsample, no half-res halo.
// ===========================================================================
TEST_CASE("fog scene: monolith silhouette stays sharp at half-res (no halo)", "[fog][gpu]") {
  FogScene base;
  base.cam_pos = glm::vec3(0.0f, 32.0f, 0.0f);
  base.cam_dir = glm::vec3(1.0f, 0.0f, 0.0f);
  base.sun_dir = glm::vec3(0.0f, 1.0f, 0.0f);
  base.box_min = glm::vec3(5.0f, 0.0f, -64.0f);   // dense fog wall
  base.box_max = glm::vec3(80.0f, 64.0f, 64.0f);
  base.sigma_t = 0.05f;
  base.fog_max_distance = 90.0f;
  base.pillar = true;
  base.pillar_rect = glm::vec4(-0.12f, -1.0f, 0.12f, 1.0f);  // centre strip
  base.pillar_depth = 0.1f;                                  // ~1 m (in front of the fog)
  base.extract_from = ExtractFrom::kComposited;
  base.bg_color = glm::vec3(0.0f);  // black -> extract = pure upsampled in-scatter
  base.extract_channel = 4.0f;

  const int x_bg = static_cast<int>(0.20f * kW);   // background fog
  const int x_edge = static_cast<int>(0.45f * kW); // just inside the pillar's left edge

  FogScene full = base; full.half_res = false; full.png_name = "monolith";
  FogScene half = base; half.half_res = true;
  CpuImage f = RenderFogScene(full);
  CpuImage h = RenderFogScene(half);
  const float bg = f.GetFloat(x_bg, kH / 2);
  const float edge_full = f.GetFloat(x_edge, kH / 2);
  const float edge_half = h.GetFloat(x_edge, kH / 2);
  spdlog::info("monolith: bg={} edge_full={} edge_half={}", bg, edge_full, edge_half);
  REQUIRE(bg > 1e-3f);                       // background fog is bright
  REQUIRE(edge_full < bg * 0.25f);           // pillar occludes -> dark
  REQUIRE(edge_half < bg * 0.25f);           // no bright halo bleed at half-res
  REQUIRE(edge_half == Catch::Approx(edge_full).margin(bg * 0.15f + 1e-3f));
}

// ===========================================================================
// Scene 4 — Sunset: Henyey-Greenstein forward scattering.
// ===========================================================================
TEST_CASE("fog scene: forward scattering brightens into the light", "[fog][gpu]") {
  FogScene base;
  base.cam_pos = glm::vec3(0.0f, 32.0f, 0.0f);
  base.cam_dir = glm::vec3(1.0f, 0.0f, 0.0f);
  base.box_min = glm::vec3(5.0f, 0.0f, -64.0f);
  base.box_max = glm::vec3(80.0f, 64.0f, 64.0f);
  base.sigma_t = 0.05f;
  base.fog_max_distance = 90.0f;
  base.phase_g = 0.85f;
  base.extract_channel = 4.0f;

  FogScene with_light = base; with_light.sun_dir = glm::vec3(-1.0f, 0.0f, 0.0f);  // behind camera
  FogScene into_light = base; into_light.sun_dir = glm::vec3(1.0f, 0.0f, 0.0f);   // ahead
  into_light.png_name = "sunset";
  const float a = RenderFogScene(with_light).GetFloat(kW / 2, kH / 2);
  const float b = RenderFogScene(into_light).GetFloat(kW / 2, kH / 2);
  spdlog::info("sunset: with_light={} into_light={}", a, b);
  REQUIRE(b > a * 10.0f);  // forward-scatter peak when looking into the sun
}

// ===========================================================================
// Scene 5 — Neon Gradient: decoupled sigma_s/sigma_t + premultiplied alpha.
// ===========================================================================
TEST_CASE("fog scene: neon gradient tints by height and composites correctly", "[fog][gpu]") {
  FogScene base;
  base.cam_pos = glm::vec3(0.0f, 5.0f, 0.0f);
  base.cam_dir = glm::vec3(1.0f, 0.0f, 0.0f);
  base.sun_dir = glm::vec3(0.0f, 1.0f, 0.0f);
  base.fill_mode = FillMode::kGradient;
  base.box_min = glm::vec3(8.0f, 0.0f, -40.0f);
  base.box_max = glm::vec3(45.0f, 10.0f, 40.0f);
  base.sigma_t = 0.05f;
  base.grad_y0 = 0.0f; base.grad_col0 = glm::vec3(1.0f, 0.0f, 0.0f);  // red at floor
  base.grad_y1 = 10.0f; base.grad_col1 = glm::vec3(0.0f, 0.0f, 1.0f);  // blue at top
  base.fog_max_distance = 50.0f;
  base.step_count = 128;

  const int y_low = static_cast<int>(0.70f * kH);  // ray dips low -> red
  const int y_high = static_cast<int>(0.30f * kH); // ray rises -> blue

  FogScene red_ch = base; red_ch.extract_channel = 0.0f; red_ch.png_name = "neon";
  FogScene blue_ch = base; blue_ch.extract_channel = 2.0f;
  CpuImage r = RenderFogScene(red_ch);
  CpuImage bl = RenderFogScene(blue_ch);
  const float low_r = r.GetFloat(kW / 2, y_low), low_b = bl.GetFloat(kW / 2, y_low);
  const float high_r = r.GetFloat(kW / 2, y_high), high_b = bl.GetFloat(kW / 2, y_high);
  spdlog::info("neon: low(r={},b={}) high(r={},b={})", low_r, low_b, high_r, high_b);
  REQUIRE(low_r > low_b);    // bottom fog scatters red
  REQUIRE(high_b > high_r);  // top fog scatters blue

  // Premultiplied-alpha compositing over a bright background: green channel is
  // just the darkened background (bg.g * T), never blackened or blown out.
  FogScene comp = base;
  comp.extract_from = ExtractFrom::kComposited;
  comp.bg_color = glm::vec3(1.0f, 1.0f, 1.0f);
  comp.extract_channel = 1.0f;  // green
  const float g = RenderFogScene(comp).GetFloat(kW / 2, y_low);
  spdlog::info("neon composite green={}", g);
  REQUIRE(g > 0.02f);   // background not blackened
  REQUIRE(g < 0.99f);   // background attenuated by the fog (not untouched/blown)
}

// ===========================================================================
// Scene 6 — Empty Sky: the fog band is bounded; the sky above is clear.
// ===========================================================================
TEST_CASE("fog scene: fog is bounded to the height band (clear sky above)", "[fog][gpu]") {
  // Camera ABOVE the band: rays that stay above y=10 must be perfectly clear;
  // only rays dipping into the band pick up fog.
  FogScene s;
  s.cam_pos = glm::vec3(0.0f, 15.0f, 0.0f);
  s.cam_dir = glm::vec3(1.0f, 0.0f, 0.0f);  // horizontal (camera is above the band)
  s.cam_up = glm::vec3(0.0f, 1.0f, 0.0f);
  s.box_min = glm::vec3(-64.0f, 0.0f, -64.0f);
  s.box_max = glm::vec3(64.0f, 10.0f, 64.0f);  // fog only in y[0,10]
  s.sigma_t = 0.05f;
  s.fog_max_distance = 200.0f;
  s.extract_channel = 3.0f;  // transmittance
  s.png_name = "empty_sky";
  CpuImage img = RenderFogScene(s);

  const float up = img.GetFloat(kW / 2, kH / 6);        // ray rises -> stays above band
  const float horizon = img.GetFloat(kW / 2, kH / 2);   // horizontal at y=15 -> above band
  const float down = img.GetFloat(kW / 2, 5 * kH / 6);  // ray dips into the band -> fog
  spdlog::info("empty sky: up={} horizon={} down={}", up, horizon, down);
  REQUIRE(up > 0.97f);       // clear sky above the ceiling
  REQUIRE(horizon > 0.97f);  // camera above the band -> horizontal is clear
  REQUIRE(down < 0.9f);      // rays dipping into the band accumulate fog
}
