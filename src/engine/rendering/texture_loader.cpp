// New (not a direct sampo port) — see texture_loader.hpp's file-level
// comment for the sanctioned trim of sampo's ProcessingGraph/ResizeNode/
// MipmapGenerationNode DAG this inlines. The render-path downsample loop
// below mirrors the logic of sampo's resize_task.cpp ExecuteBoxResize()
// render branch + mipmap_generation_task.cpp's per-level loop, but built
// directly against badlands' explicit-reflection GpuPipelineGenerator
// instead of sampo's AUTO bind-group-layout API
// (pipeline->pipeline.GetBindGroupLayout(0)) — badlands uses
// CreateBindGroup(device, *pipeline, group, entries) instead (see
// gpu_pipeline_generator.hpp).
#include "engine/rendering/texture_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "badlands_assets.h"

namespace badlands {

namespace {

// RAII guard: frees a BadlandsImage's pixel buffer on every exit path
// (badlands_image_free is safe to call on a failure result too).
struct ImageGuard {
  BadlandsImage image;
  ~ImageGuard() { badlands_image_free(image); }
};

}  // namespace

LoadedTexture LoadTexture2D(wgpu::Device device, wgpu::Queue queue,
                            GpuPipelineGenerator& pipeline_gen,
                            const std::string& path) {
  // 1. Decode.
  BadlandsImage img = badlands_decode_jpeg(path.c_str());
  ImageGuard guard{img};
  if (img.rgba == nullptr) {
    spdlog::error("LoadTexture2D: failed to decode JPEG '{}'", path);
    return {};
  }

  // 2. Texture (full mip chain sized to the larger dimension).
  uint32_t mip_level_count =
      static_cast<uint32_t>(
          std::floor(std::log2(std::max(img.width, img.height)))) +
      1;

  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {img.width, img.height, 1};
  tex_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  tex_desc.usage = wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::RenderAttachment |
                   wgpu::TextureUsage::CopyDst;
  tex_desc.mipLevelCount = mip_level_count;
  tex_desc.sampleCount = 1;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture texture = device.CreateTexture(&tex_desc);
  if (!texture) {
    spdlog::error("LoadTexture2D: failed to create texture for '{}'", path);
    return {};
  }

  // 3. Upload level 0. WriteTexture has no 256-byte row-alignment
  // requirement (that only applies to buffer<->texture copies), so the
  // tightly-packed decode output can be uploaded directly.
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = texture;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};

  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = img.width * 4;
  layout.rowsPerImage = img.height;

  wgpu::Extent3D extent = {img.width, img.height, 1};
  queue.WriteTexture(&dst, img.rgba,
                     static_cast<size_t>(img.width) * img.height * 4, &layout,
                     &extent);

  // 4. Generate mips (render-path box downsample), one level at a time.
  // The pipeline declaration is identical every iteration (and results are
  // cached by GpuPipelineGenerator anyway) so it's compiled once, outside
  // the loop; likewise the sampler is created once and reused.
  RenderPipelineDeclaration decl;
  decl.shader_path = "compute/mip_generator_render";
  decl.vertex_layout = VertexLayout::kFullscreen;
  decl.topology = wgpu::PrimitiveTopology::TriangleList;
  // features left empty -> is_cubemap false (2D branch).

  RenderTargetFormats target_formats = {wgpu::TextureFormat::RGBA8Unorm};
  auto pipeline = pipeline_gen.GetPipeline(decl, target_formats);
  if (!pipeline) {
    spdlog::error(
        "LoadTexture2D: failed to compile mip_generator_render pipeline "
        "for '{}'",
        path);
    return {};
  }

  wgpu::SamplerDescriptor sampler_desc;
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  sampler_desc.maxAnisotropy = 1;
  wgpu::Sampler sampler = device.CreateSampler(&sampler_desc);

  // All levels go through one command encoder, submitted once after the
  // loop. WriteTexture above is queue-ordered before this submission, so
  // level 0 is already populated when the first pass samples it.
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  for (uint32_t i = 0; i + 1 < mip_level_count; ++i) {
    wgpu::TextureViewDescriptor src_view_desc;
    src_view_desc.baseMipLevel = i;
    src_view_desc.mipLevelCount = 1;
    src_view_desc.dimension = wgpu::TextureViewDimension::e2D;
    wgpu::TextureView src_view = texture.CreateView(&src_view_desc);

    wgpu::TextureViewDescriptor dst_view_desc;
    dst_view_desc.baseMipLevel = i + 1;
    dst_view_desc.mipLevelCount = 1;
    dst_view_desc.dimension = wgpu::TextureViewDimension::e2D;
    wgpu::TextureView dst_view = texture.CreateView(&dst_view_desc);

    std::array<wgpu::BindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].textureView = src_view;
    entries[1].binding = 1;
    entries[1].sampler = sampler;

    wgpu::BindGroup bind_group =
        CreateBindGroup(device, *pipeline, /*group=*/0, entries);

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = dst_view;
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;

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

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // 5. Return a view over all mip levels.
  wgpu::TextureViewDescriptor full_view_desc;
  full_view_desc.baseMipLevel = 0;
  full_view_desc.mipLevelCount = mip_level_count;
  full_view_desc.dimension = wgpu::TextureViewDimension::e2D;
  wgpu::TextureView view = texture.CreateView(&full_view_desc);

  return LoadedTexture{.texture = texture, .view = view};
}

}  // namespace badlands
