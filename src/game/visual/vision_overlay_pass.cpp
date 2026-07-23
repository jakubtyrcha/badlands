#include "game/visual/vision_overlay_pass.hpp"

#include <array>
#include <cstdint>

#include "engine/core/camera.hpp"  // UniformData (frame UBO size)
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

// Matches VisionParams in shaders/game/vision.wesl.
struct VisionGpuParams {
  glm::vec4 origin_inv_size;  // xy = grid origin (world), zw = world->uv scale
  glm::vec4 flags;            // x = enabled
};

bool VisionOverlayPass::Initialize(wgpu::Device device, wgpu::Queue queue,
                                   GpuPipelineGenerator* pipeline_gen) {
  device_ = device;
  queue_ = queue;
  pipeline_gen_ = pipeline_gen;
  if (!device_ || !queue_ || pipeline_gen_ == nullptr) return false;

  wgpu::SamplerDescriptor s;
  s.magFilter = wgpu::FilterMode::Linear;
  s.minFilter = wgpu::FilterMode::Linear;
  s.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  s.addressModeU = wgpu::AddressMode::ClampToEdge;
  s.addressModeV = wgpu::AddressMode::ClampToEdge;
  s.addressModeW = wgpu::AddressMode::ClampToEdge;
  sampler_ = device_.CreateSampler(&s);
  return true;
}

void VisionOverlayPass::Upload(const VisionField& field) {
  if (field.rg == nullptr || field.nx <= 0 || field.nz <= 0) {
    has_field_ = false;
    return;
  }

  if (field.nx != nx_ || field.nz != nz_ || !texture_) {
    wgpu::TextureDescriptor td;
    td.size = {static_cast<uint32_t>(field.nx), static_cast<uint32_t>(field.nz), 1};
    td.format = wgpu::TextureFormat::RG8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.dimension = wgpu::TextureDimension::e2D;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    texture_ = device_.CreateTexture(&td);
    view_ = texture_.CreateView();
    nx_ = field.nx;
    nz_ = field.nz;
  }

  origin_ = glm::vec2(field.world_min_x, field.world_min_z) + sim_to_world_;
  const float size_x = static_cast<float>(field.nx) * field.texel_m;
  const float size_z = static_cast<float>(field.nz) * field.texel_m;
  inv_size_ = glm::vec2(1.0f / size_x, 1.0f / size_z);

  wgpu::TexelCopyTextureInfo dst;
  dst.texture = texture_;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  wgpu::TexelCopyBufferLayout layout;
  layout.offset = 0;
  layout.bytesPerRow = static_cast<uint32_t>(field.nx) * 2u;  // RG8, 2 bytes/texel
  layout.rowsPerImage = static_cast<uint32_t>(field.nz);
  wgpu::Extent3D extent = {static_cast<uint32_t>(field.nx),
                           static_cast<uint32_t>(field.nz), 1};
  queue_.WriteTexture(&dst, field.rg,
                      static_cast<size_t>(field.nx) * static_cast<size_t>(field.nz) * 2u,
                      &layout, &extent);
  has_field_ = true;
}

void VisionOverlayPass::Execute(const PostSceneContext& ctx) {
  if (!enabled_ || !has_field_ || !view_ || pipeline_gen_ == nullptr) return;

  RenderPipelineDeclaration decl;
  decl.shader_path = "game/vision";
  // Fullscreen triangle, no depth, no blend (overwrites every pixel).
  RenderTargetFormats formats = {ctx.color_format};
  auto compiled = pipeline_gen_->GetPipeline(decl, formats);
  if (!compiled) return;

  VisionGpuParams params;
  params.origin_inv_size = glm::vec4(origin_.x, origin_.y, inv_size_.x, inv_size_.y);
  params.flags = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
  wgpu::Buffer params_buf = ctx.frame.CreateUniformBuffer(sizeof(params), &params);

  std::array<wgpu::BindGroupEntry, 6> entries{};
  entries[0].binding = 0;
  entries[0].buffer = ctx.frame.GetFrameUniformBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(UniformData);
  entries[1].binding = 1;
  entries[1].textureView = ctx.color_source;
  entries[2].binding = 2;
  entries[2].textureView = ctx.depth;
  entries[3].binding = 3;
  entries[3].textureView = view_;
  entries[4].binding = 4;
  entries[4].sampler = sampler_;
  entries[5].binding = 5;
  entries[5].buffer = params_buf;
  entries[5].offset = 0;
  entries[5].size = sizeof(params);

  wgpu::BindGroup bind_group =
      ctx.frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = ctx.color_target;
  color_attachment.loadOp = wgpu::LoadOp::Load;
  color_attachment.storeOp = wgpu::StoreOp::Store;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;
  desc.depthStencilAttachment = nullptr;

  RenderPassContext pass = ctx.frame.BeginRenderPass(desc);
  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.Draw(3);
  pass.End();
}

}  // namespace badlands
