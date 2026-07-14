#pragma once

// Deferred-rendering G-buffer: the geometry pass's MRT color targets +
// reversed-Z depth. Trimmed port of sampo's src/rendering/gbuffer.{hpp,cpp}
// (namespace sampo -> badlands): drops the lazily-allocated motion-vector
// target (TAA, not ported) — task S2.B1 needs only the three color targets +
// depth. All targets carry `TextureBinding` usage because the deferred
// lighting pass samples every one of them (via textureLoad).
#include <cstdint>
#include <dawn/webgpu_cpp.h>

namespace badlands {

class GBuffer {
 public:
  // Fixed G-buffer target formats (must match the `normalmapped` kGBuffer
  // pipeline's color/depth formats and the deferred_lighting pass's samples):
  //   normals  RG16Float   octahedron-encoded world normal
  //   albedo   BGRA8Unorm  base color
  //   material RGBA8Unorm  R=roughness, G=shadow, B/A=reserved
  //   depth    Depth32Float reversed-Z (0 = far)
  static constexpr wgpu::TextureFormat kNormalsFormat =
      wgpu::TextureFormat::RG16Float;
  static constexpr wgpu::TextureFormat kAlbedoFormat =
      wgpu::TextureFormat::BGRA8Unorm;
  static constexpr wgpu::TextureFormat kMaterialFormat =
      wgpu::TextureFormat::RGBA8Unorm;
  static constexpr wgpu::TextureFormat kDepthFormat =
      wgpu::TextureFormat::Depth32Float;

  GBuffer() = default;

  // (Re)allocates all targets at (width, height). Safe to call repeatedly;
  // Dawn's wgpu::Texture is ref-counted, so overwriting the old textures
  // releases them.
  void Create(wgpu::Device device, uint32_t width, uint32_t height);

  // Recreates targets at a new size. No-op if unchanged or zero-sized.
  void Resize(uint32_t width, uint32_t height);

  bool IsValid() const { return depth_texture_ != nullptr; }
  uint32_t GetWidth() const { return width_; }
  uint32_t GetHeight() const { return height_; }

  wgpu::TextureView GetNormalsView() const { return normals_view_; }
  wgpu::TextureView GetAlbedoView() const { return albedo_view_; }
  wgpu::TextureView GetMaterialView() const { return material_view_; }
  wgpu::TextureView GetDepthView() const { return depth_view_; }

 private:
  wgpu::Device device_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  wgpu::Texture normals_texture_;
  wgpu::TextureView normals_view_;
  wgpu::Texture albedo_texture_;
  wgpu::TextureView albedo_view_;
  wgpu::Texture material_texture_;
  wgpu::TextureView material_view_;
  wgpu::Texture depth_texture_;
  wgpu::TextureView depth_view_;
};

}  // namespace badlands
