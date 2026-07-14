#include "engine/rendering/brdf_lut.hpp"

#include <spdlog/spdlog.h>

#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

bool BrdfLut::Generate(wgpu::Device device, wgpu::Queue queue,
                       GpuPipelineGenerator& pipeline_gen) {
  // RG16Float target (renderable), also sampled by deferred_lighting.
  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {kLutSize, kLutSize, 1};
  tex_desc.format = kFormat;
  tex_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  tex_desc.mipLevelCount = 1;
  tex_desc.sampleCount = 1;
  texture_ = device.CreateTexture(&tex_desc);
  if (!texture_) {
    spdlog::error("BrdfLut::Generate: failed to create LUT texture");
    return false;
  }
  view_ = texture_.CreateView();

  wgpu::SamplerDescriptor sampler_desc;
  sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
  sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
  sampler_desc.addressModeW = wgpu::AddressMode::ClampToEdge;
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  sampler_desc.maxAnisotropy = 1;
  sampler_ = device.CreateSampler(&sampler_desc);

  // Fullscreen render into the LUT. The shader declares no bindings, so no
  // bind group is set.
  RenderPipelineDeclaration decl;
  decl.shader_path = "ibl/brdf_lut_render";
  decl.vertex_layout = VertexLayout::kFullscreen;

  RenderTargetFormats target_formats = {kFormat};
  auto pipeline = pipeline_gen.GetPipeline(decl, target_formats);
  if (!pipeline) {
    spdlog::error("BrdfLut::Generate: failed to compile brdf_lut_render");
    return false;
  }

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = view_;
  color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color_attachment.loadOp = wgpu::LoadOp::Clear;
  color_attachment.storeOp = wgpu::StoreOp::Store;
  color_attachment.clearValue = {0.0, 0.0, 0.0, 1.0};

  wgpu::RenderPassDescriptor pass_desc;
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &color_attachment;
  pass_desc.depthStencilAttachment = nullptr;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(pipeline->pipeline);
  pass.Draw(3, 1, 0, 0);
  pass.End();

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  spdlog::info("BRDF LUT generated ({}x{})", kLutSize, kLutSize);
  return true;
}

}  // namespace badlands
