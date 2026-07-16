#include "engine/rendering/color_render_target.hpp"

namespace badlands {

ColorRenderTarget::ColorRenderTarget(wgpu::Device device, uint32_t width,
                                     uint32_t height,
                                     wgpu::TextureFormat format)
    : width_(width), height_(height), format_(format) {
  // Color texture
  wgpu::TextureDescriptor desc;
  desc.size = {width, height, 1};
  desc.format = format;
  desc.usage = wgpu::TextureUsage::RenderAttachment |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::TextureBinding;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::e2D;

  texture_ = device.CreateTexture(&desc);
  view_ = texture_.CreateView();

  // Depth texture (readable/sampleable for depth-consuming passes)
  wgpu::TextureDescriptor depth_desc;
  depth_desc.size = {width, height, 1};
  depth_desc.format = wgpu::TextureFormat::Depth32Float;
  depth_desc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc;
  depth_desc.mipLevelCount = 1;
  depth_desc.sampleCount = 1;
  depth_desc.dimension = wgpu::TextureDimension::e2D;

  depth_texture_ = device.CreateTexture(&depth_desc);
  depth_view_ = depth_texture_.CreateView();
}

ColorRenderTarget::ColorRenderTarget(ColorRenderTarget&& other) noexcept
    : texture_(other.texture_),
      view_(other.view_),
      depth_texture_(other.depth_texture_),
      depth_view_(other.depth_view_),
      width_(other.width_),
      height_(other.height_),
      format_(other.format_) {
  other.texture_ = nullptr;
  other.view_ = nullptr;
  other.depth_texture_ = nullptr;
  other.depth_view_ = nullptr;
  other.width_ = 0;
  other.height_ = 0;
}

ColorRenderTarget& ColorRenderTarget::operator=(
    ColorRenderTarget&& other) noexcept {
  if (this != &other) {
    if (texture_) {
      texture_.Destroy();
    }
    if (depth_texture_) {
      depth_texture_.Destroy();
    }
    texture_ = other.texture_;
    view_ = other.view_;
    depth_texture_ = other.depth_texture_;
    depth_view_ = other.depth_view_;
    width_ = other.width_;
    height_ = other.height_;
    format_ = other.format_;
    other.texture_ = nullptr;
    other.view_ = nullptr;
    other.depth_texture_ = nullptr;
    other.depth_view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
  }
  return *this;
}

ColorRenderTarget::~ColorRenderTarget() {
  if (texture_) {
    texture_.Destroy();
  }
  if (depth_texture_) {
    depth_texture_.Destroy();
  }
}

}  // namespace badlands
