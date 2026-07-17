#include "engine/rendering/volumetric_fog.hpp"

#include <array>
#include <climits>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "engine/core/camera.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/gpu_timer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {
namespace {

// GPU-side mirror of shaders/compute/fog_fill.wesl's FogFillParams. 16 scalars,
// 64 bytes (std140-safe: all 4-byte scalars, size a multiple of 16). Media is
// now generated from analytic shapes in the shader, so there are no per-fill
// density/extinction/colour params.
struct FogFillParams {
  int32_t min_voxel_x, min_voxel_z, box_off_x, box_off_z;
  int32_t box_size_x, box_size_z, res_xz, res_y;
  int32_t cascade_index, cascade_count, pad0, pad1;
  float voxel_size_xz, voxel_size_y, floor_y, height;
};
static_assert(sizeof(FogFillParams) == 64, "FogFillParams must match WGSL");

// GPU-side mirror of shaders/common/fog_types.wesl's FogUniforms. 16 f32,
// 64 bytes.
struct FogUniformsGpu {
  float fog_max_distance, phase_g, floor_y, height;
  float base_half_extent, ambient_scale, sun_scale, step_count;
  float res_y, cascade_count, enable_shafts, jitter_enabled;
  float fog_render_w, fog_render_h, frame_index, pad0;
};
static_assert(sizeof(FogUniformsGpu) == 64, "FogUniforms must match WGSL");

}  // namespace

void VolumetricFog::Initialize(wgpu::Device device, GpuPipelineGenerator* gen,
                               wgpu::TextureFormat hdr_format) {
  device_ = device;
  pipeline_generator_ = gen;
  hdr_format_ = hdr_format;
}

void VolumetricFog::EnsureTextures() {
  const fog::CascadeLayout L = config_.Layout();
  const bool dims_changed =
      !textures_valid_ || L.cascade_count != built_layout_.cascade_count ||
      L.res_xz != built_layout_.res_xz || L.res_y != built_layout_.res_y;
  const bool geom_changed =
      L.base_half_extent != built_layout_.base_half_extent ||
      L.height != built_layout_.height || L.floor_y != built_layout_.floor_y;

  if (dims_changed) {
    wgpu::TextureDescriptor desc;
    desc.dimension = wgpu::TextureDimension::e3D;
    desc.size = {static_cast<uint32_t>(L.res_xz),
                 static_cast<uint32_t>(L.res_xz),
                 static_cast<uint32_t>(L.res_y * L.cascade_count)};
    desc.format = wgpu::TextureFormat::RGBA16Float;  // core storage format, no gate
    desc.usage = wgpu::TextureUsage::StorageBinding |
                 wgpu::TextureUsage::TextureBinding;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    media_texture_ = device_.CreateTexture(&desc);
    media_view_ = media_texture_.CreateView();

    if (!media_sampler_) {
      wgpu::SamplerDescriptor s;
      // Repeat in X/Z = the toroidal wrap; clamp the packed Y/cascade axis.
      s.addressModeU = wgpu::AddressMode::Repeat;
      s.addressModeV = wgpu::AddressMode::Repeat;
      s.addressModeW = wgpu::AddressMode::ClampToEdge;
      s.magFilter = wgpu::FilterMode::Linear;
      s.minFilter = wgpu::FilterMode::Linear;
      s.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
      media_sampler_ = device_.CreateSampler(&s);
    }

    last_min_voxel_.assign(L.cascade_count, glm::ivec2(INT_MIN, INT_MIN));
    textures_valid_ = true;
  }
  if (dims_changed || geom_changed) {
    force_fill_ = true;
  }
  built_layout_ = L;
}

void VolumetricFog::EnsureFogTarget(uint32_t render_w, uint32_t render_h) {
  if (fog_target_texture_ && render_w == fog_target_w_ &&
      render_h == fog_target_h_) {
    return;
  }
  wgpu::TextureDescriptor desc;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.size = {render_w, render_h, 1};
  desc.format = wgpu::TextureFormat::RGBA16Float;
  desc.usage = wgpu::TextureUsage::RenderAttachment |
               wgpu::TextureUsage::TextureBinding;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  fog_target_texture_ = device_.CreateTexture(&desc);
  fog_target_view_ = fog_target_texture_.CreateView();
  fog_target_w_ = render_w;
  fog_target_h_ = render_h;
}

void VolumetricFog::Render(FrameContext& frame, GpuTimer& gpu_timer,
                           const Camera& camera, wgpu::TextureView hdr_view,
                           wgpu::TextureView depth_view,
                           wgpu::TextureView shadow_view,
                           wgpu::Sampler shadow_sampler, uint32_t width,
                           uint32_t height) {
  if (!config_.enabled || !pipeline_generator_) {
    return;
  }
  EnsureTextures();
  const fog::CascadeLayout L = config_.Layout();

  const uint32_t render_w =
      config_.half_res ? std::max(1u, (width + 1u) / 2u) : width;
  const uint32_t render_h =
      config_.half_res ? std::max(1u, (height + 1u) / 2u) : height;
  EnsureFogTarget(render_w, render_h);

  auto fill_pipe = pipeline_generator_->GetComputePipeline("compute/fog_fill");

  // Raymarch pipeline: writes premultiplied (in-scatter, transmittance) to the
  // fog target (no blend).
  RenderPipelineDeclaration raymarch_decl;
  raymarch_decl.shader_path = "passes/fog";
  auto raymarch_pipe = pipeline_generator_->GetPipeline(
      raymarch_decl, {wgpu::TextureFormat::RGBA16Float});

  // Composite pipeline: depth-aware upsample + premultiplied-over blend into
  // HDR (out = accum*1 + hdr*transmittance; transmittance in src alpha).
  RenderPipelineDeclaration comp_decl;
  comp_decl.shader_path = "passes/fog_composite";
  wgpu::BlendState blend;
  blend.color.operation = wgpu::BlendOperation::Add;
  blend.color.srcFactor = wgpu::BlendFactor::One;
  blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;
  blend.alpha.operation = wgpu::BlendOperation::Add;
  blend.alpha.srcFactor = wgpu::BlendFactor::Zero;
  blend.alpha.dstFactor = wgpu::BlendFactor::One;
  comp_decl.custom_blend = blend;
  auto composite_pipe =
      pipeline_generator_->GetPipeline(comp_decl, {hdr_format_});

  if (!fill_pipe || !fill_pipe->pipeline || !raymarch_pipe ||
      !raymarch_pipe->pipeline || !composite_pipe ||
      !composite_pipe->pipeline) {
    if (!logged_compile_error_) {
      spdlog::error("VolumetricFog: failed to compile fog pipelines");
      logged_compile_error_ = true;
    }
    return;
  }

  const glm::vec3 cam = camera.GetPosition();

  // === Fill (compute): refill newly-scrolled voxel columns per cascade. ===
  {
    wgpu::ComputePassEncoder cp =
        frame.BeginComputePass(gpu_timer.BeginPass("fog_fill"));
    cp.SetPipeline(fill_pipe->pipeline);
    const uint32_t wx = std::max(1u, fill_pipe->workgroup_size[0]);
    const uint32_t wy = std::max(1u, fill_pipe->workgroup_size[1]);
    const uint32_t wz = std::max(1u, fill_pipe->workgroup_size[2]);

    for (int i = 0; i < L.cascade_count; ++i) {
      const float voxel = fog::CascadeVoxelSizeXZ(L, i);
      const glm::ivec2 new_min(fog::CascadeMinVoxel(L, i, cam.x),
                               fog::CascadeMinVoxel(L, i, cam.z));
      const auto boxes = fog::ComputeDirtyBoxes(L.res_xz, last_min_voxel_[i],
                                                new_min, force_fill_);
      for (const auto& b : boxes) {
        FogFillParams p{};
        p.min_voxel_x = new_min.x;
        p.min_voxel_z = new_min.y;
        p.box_off_x = b.ox;
        p.box_off_z = b.oz;
        p.box_size_x = b.sx;
        p.box_size_z = b.sz;
        p.res_xz = L.res_xz;
        p.res_y = L.res_y;
        p.cascade_index = i;
        p.cascade_count = L.cascade_count;
        p.voxel_size_xz = voxel;
        p.voxel_size_y = fog::CascadeVoxelSizeY(L);
        p.floor_y = L.floor_y;
        p.height = L.height;

        wgpu::Buffer ubo = frame.CreateUniformBuffer(sizeof(p), &p);
        std::array<wgpu::BindGroupEntry, 2> e{};
        e[0].binding = 0;
        e[0].buffer = ubo;
        e[0].offset = 0;
        e[0].size = sizeof(p);
        e[1].binding = 1;
        e[1].textureView = media_view_;
        wgpu::BindGroup bg =
            frame.CreateBindGroup(fill_pipe->bind_group_layouts[0], e);
        cp.SetBindGroup(0, bg, 0, nullptr);

        const uint32_t dx = (static_cast<uint32_t>(b.sx) + wx - 1) / wx;
        const uint32_t dy = (static_cast<uint32_t>(b.sz) + wy - 1) / wy;
        const uint32_t dz = (static_cast<uint32_t>(L.res_y) + wz - 1) / wz;
        cp.DispatchWorkgroups(dx, dy, dz);
      }
      last_min_voxel_[i] = new_min;
    }
    cp.End();
  }
  force_fill_ = false;

  // Shared fog uniforms (both render passes bind this at group 1, binding 0).
  FogUniformsGpu u{};
  u.fog_max_distance = config_.fog_max_distance;
  u.phase_g = config_.phase_g;
  u.floor_y = L.floor_y;
  u.height = L.height;
  u.base_half_extent = L.base_half_extent;
  u.ambient_scale = config_.ambient_scale;
  u.sun_scale = config_.sun_scale;
  u.step_count = static_cast<float>(config_.step_count);
  u.res_y = static_cast<float>(L.res_y);
  u.cascade_count = static_cast<float>(L.cascade_count);
  u.enable_shafts = config_.enable_shafts ? 1.0f : 0.0f;
  u.jitter_enabled = config_.jitter ? 1.0f : 0.0f;
  u.fog_render_w = static_cast<float>(render_w);
  u.fog_render_h = static_cast<float>(render_h);
  u.frame_index = static_cast<float>(frame_index_);
  wgpu::Buffer ubo = frame.CreateUniformBuffer(sizeof(u), &u);
  // GetPipeline forces group-1 buffers to dynamic offsets (material-system
  // convention); FogUniforms is the one dynamic buffer -> offset 0.
  const uint32_t fog_dyn_offset = 0;

  // === Raymarch (fullscreen at fog target res): write (in-scatter, T). ===
  {
    std::array<wgpu::BindGroupEntry, 1> g0{};
    g0[0].binding = 0;
    g0[0].buffer = frame.GetFrameUniformBuffer();
    g0[0].offset = 0;
    g0[0].size = sizeof(UniformData);
    wgpu::BindGroup bg0 =
        frame.CreateBindGroup(raymarch_pipe->bind_group_layouts[0], g0);

    std::array<wgpu::BindGroupEntry, 6> g1{};
    g1[0].binding = 0;
    g1[0].buffer = ubo;
    g1[0].offset = 0;
    g1[0].size = sizeof(u);
    g1[1].binding = 1;
    g1[1].textureView = media_view_;
    g1[2].binding = 2;
    g1[2].sampler = media_sampler_;
    g1[3].binding = 3;
    g1[3].textureView = depth_view;
    g1[4].binding = 4;
    g1[4].textureView = shadow_view;
    g1[5].binding = 5;
    g1[5].sampler = shadow_sampler;
    wgpu::BindGroup bg1 =
        frame.CreateBindGroup(raymarch_pipe->bind_group_layouts[1], g1);

    wgpu::RenderPassColorAttachment ca;
    ca.view = fog_target_view_;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 1.0};  // no fog: transmittance 1

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = nullptr;
    desc.timestampWrites = gpu_timer.BeginPass("fog_raymarch");

    RenderPassContext pass = frame.BeginRenderPass(desc);
    pass.SetPipeline(raymarch_pipe->pipeline);
    pass.SetBindGroup(0, bg0);
    pass.SetBindGroup(1, bg1, 1, &fog_dyn_offset);
    pass.Draw(3);
    pass.End();
  }

  // === Composite (fullscreen at screen res): depth-aware upsample + blend. ===
  {
    std::array<wgpu::BindGroupEntry, 1> g0{};
    g0[0].binding = 0;
    g0[0].buffer = frame.GetFrameUniformBuffer();
    g0[0].offset = 0;
    g0[0].size = sizeof(UniformData);
    wgpu::BindGroup bg0 =
        frame.CreateBindGroup(composite_pipe->bind_group_layouts[0], g0);

    std::array<wgpu::BindGroupEntry, 3> g1{};
    g1[0].binding = 0;
    g1[0].buffer = ubo;
    g1[0].offset = 0;
    g1[0].size = sizeof(u);
    g1[1].binding = 1;
    g1[1].textureView = fog_target_view_;
    g1[2].binding = 2;
    g1[2].textureView = depth_view;
    wgpu::BindGroup bg1 =
        frame.CreateBindGroup(composite_pipe->bind_group_layouts[1], g1);

    wgpu::RenderPassColorAttachment ca;
    ca.view = hdr_view;
    ca.loadOp = wgpu::LoadOp::Load;
    ca.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = nullptr;
    desc.timestampWrites = gpu_timer.BeginPass("fog");

    RenderPassContext pass = frame.BeginRenderPass(desc);
    pass.SetPipeline(composite_pipe->pipeline);
    pass.SetBindGroup(0, bg0);
    pass.SetBindGroup(1, bg1, 1, &fog_dyn_offset);
    pass.Draw(3);
    pass.End();
  }

  ++frame_index_;
}

}  // namespace badlands
