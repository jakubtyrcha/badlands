#include "engine/rendering/prefiltered_cubemap.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <spdlog/spdlog.h>

#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

namespace {

// Matches shaders/ibl/prefilter_render.wesl's PrefilterParams (std140, 16B).
struct PrefilterParams {
  float roughness;
  uint32_t face_index;
  uint32_t output_size;
  uint32_t _padding;
};

}  // namespace

bool PrefilteredCubemap::Generate(wgpu::Device device, wgpu::Queue queue,
                                  GpuPipelineGenerator& pipeline_gen,
                                  wgpu::TextureView source_cube_view,
                                  wgpu::Sampler source_sampler,
                                  uint32_t face_size, uint32_t mip_count) {
  if (!source_cube_view || face_size == 0 || mip_count == 0) {
    spdlog::error("PrefilteredCubemap::Generate: invalid arguments");
    return false;
  }
  face_size_ = face_size;
  mip_count_ = mip_count;

  // RGBA16Float cube, renderable + sampleable, small mip chain.
  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {face_size, face_size, 6};
  tex_desc.format = kFormat;
  tex_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  tex_desc.mipLevelCount = mip_count;
  tex_desc.sampleCount = 1;
  texture_ = device.CreateTexture(&tex_desc);
  if (!texture_) {
    spdlog::error("PrefilteredCubemap::Generate: failed to create cube texture");
    return false;
  }

  wgpu::SamplerDescriptor sampler_desc;
  sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
  sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
  sampler_desc.addressModeW = wgpu::AddressMode::ClampToEdge;
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;  // trilinear
  sampler_desc.maxAnisotropy = 1;
  sampler_ = device.CreateSampler(&sampler_desc);

  // Cube view over all mips (what deferred_lighting samples).
  wgpu::TextureViewDescriptor cube_view_desc;
  cube_view_desc.format = kFormat;
  cube_view_desc.dimension = wgpu::TextureViewDimension::Cube;
  cube_view_desc.baseMipLevel = 0;
  cube_view_desc.mipLevelCount = mip_count;
  cube_view_desc.baseArrayLayer = 0;
  cube_view_desc.arrayLayerCount = 6;
  view_ = texture_.CreateView(&cube_view_desc);

  // Prefilter pipeline: group 0 = source cube @0, sampler @1, params UBO @2.
  RenderPipelineDeclaration decl;
  decl.shader_path = "ibl/prefilter_render";
  decl.vertex_layout = VertexLayout::kFullscreen;

  RenderTargetFormats target_formats = {kFormat};
  auto pipeline = pipeline_gen.GetPipeline(decl, target_formats);
  if (!pipeline) {
    spdlog::error("PrefilteredCubemap::Generate: failed to compile "
                  "prefilter_render pipeline");
    return false;
  }

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  for (uint32_t mip = 0; mip < mip_count; ++mip) {
    uint32_t mip_size = std::max(1u, face_size >> mip);
    float roughness = mip_count > 1 ? static_cast<float>(mip) /
                                          static_cast<float>(mip_count - 1)
                                    : 0.0f;

    for (uint32_t face = 0; face < 6; ++face) {
      PrefilterParams params{roughness, face, mip_size, 0};

      wgpu::BufferDescriptor buf_desc;
      buf_desc.size = sizeof(PrefilterParams);
      buf_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
      buf_desc.mappedAtCreation = true;
      wgpu::Buffer params_buffer = device.CreateBuffer(&buf_desc);
      std::memcpy(params_buffer.GetMappedRange(0, sizeof(PrefilterParams)),
                  &params, sizeof(PrefilterParams));
      params_buffer.Unmap();

      // 2D render view of one face at this mip.
      wgpu::TextureViewDescriptor face_view_desc;
      face_view_desc.format = kFormat;
      face_view_desc.dimension = wgpu::TextureViewDimension::e2D;
      face_view_desc.baseMipLevel = mip;
      face_view_desc.mipLevelCount = 1;
      face_view_desc.baseArrayLayer = face;
      face_view_desc.arrayLayerCount = 1;
      wgpu::TextureView face_view = texture_.CreateView(&face_view_desc);

      std::array<wgpu::BindGroupEntry, 3> entries{};
      entries[0].binding = 0;
      entries[0].textureView = source_cube_view;
      entries[1].binding = 1;
      entries[1].sampler = source_sampler;
      entries[2].binding = 2;
      entries[2].buffer = params_buffer;
      entries[2].offset = 0;
      entries[2].size = sizeof(PrefilterParams);

      wgpu::BindGroup bind_group =
          CreateBindGroup(device, *pipeline, /*group=*/0, entries);

      wgpu::RenderPassColorAttachment color_attachment;
      color_attachment.view = face_view;
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
      pass.SetBindGroup(0, bind_group);
      pass.Draw(3, 1, 0, 0);
      pass.End();
    }
  }

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  spdlog::info("Prefiltered cubemap generated ({}x{}, {} mips)", face_size,
               face_size, mip_count);
  return true;
}

}  // namespace badlands
