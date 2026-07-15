#pragma once

#include <cstdint>

#include <dawn/webgpu_cpp.h>

namespace badlands {

// Offscreen render target texture (color + depth) that supports readback to
// CPU. Ported from sampo::ColorRenderTarget. The depth buffer is always
// Depth32Float and is CopySrc/TextureBinding so it can be read back or sampled.
class ColorRenderTarget {
 public:
  ColorRenderTarget() = default;
  ColorRenderTarget(wgpu::Device device, uint32_t width, uint32_t height,
                    wgpu::TextureFormat format);

  // Non-copyable
  ColorRenderTarget(const ColorRenderTarget&) = delete;
  ColorRenderTarget& operator=(const ColorRenderTarget&) = delete;

  // Movable
  ColorRenderTarget(ColorRenderTarget&& other) noexcept;
  ColorRenderTarget& operator=(ColorRenderTarget&& other) noexcept;

  ~ColorRenderTarget();

  uint32_t GetWidth() const { return width_; }
  uint32_t GetHeight() const { return height_; }
  wgpu::TextureFormat GetFormat() const { return format_; }

  // For rendering
  wgpu::TextureView GetView() const { return view_; }
  wgpu::TextureView GetDepthView() const { return depth_view_; }

  // For readback
  wgpu::Texture GetTexture() const { return texture_; }
  wgpu::Texture GetDepthTexture() const { return depth_texture_; }

  bool IsValid() const { return texture_ != nullptr; }

 private:
  wgpu::Texture texture_;
  wgpu::TextureView view_;
  wgpu::Texture depth_texture_;
  wgpu::TextureView depth_view_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  wgpu::TextureFormat format_ = wgpu::TextureFormat::Undefined;
};

}  // namespace badlands
