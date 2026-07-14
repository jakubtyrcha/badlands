// Trimmed port of sampo's src/rendering/gbuffer.cpp (namespace sampo ->
// badlands). See gbuffer.hpp for the deviation note (no motion vectors).
#include "engine/rendering/gbuffer.hpp"

namespace badlands {

namespace {

wgpu::Texture CreateTarget(wgpu::Device device, uint32_t width, uint32_t height,
                           wgpu::TextureFormat format) {
  wgpu::TextureDescriptor desc;
  desc.size = {width, height, 1};
  desc.format = format;
  // TextureBinding is required in addition to RenderAttachment: the deferred
  // lighting pass samples every G-buffer target (including depth).
  desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::e2D;
  return device.CreateTexture(&desc);
}

}  // namespace

void GBuffer::Create(wgpu::Device device, uint32_t width, uint32_t height) {
  device_ = device;
  width_ = width;
  height_ = height;

  normals_texture_ = CreateTarget(device, width, height, kNormalsFormat);
  normals_view_ = normals_texture_.CreateView();

  albedo_texture_ = CreateTarget(device, width, height, kAlbedoFormat);
  albedo_view_ = albedo_texture_.CreateView();

  material_texture_ = CreateTarget(device, width, height, kMaterialFormat);
  material_view_ = material_texture_.CreateView();

  depth_texture_ = CreateTarget(device, width, height, kDepthFormat);
  depth_view_ = depth_texture_.CreateView();
}

void GBuffer::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || (width == width_ && height == height_)) {
    return;
  }
  Create(device_, width, height);
}

}  // namespace badlands
