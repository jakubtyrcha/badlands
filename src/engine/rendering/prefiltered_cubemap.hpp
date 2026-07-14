#pragma once

// PrefilteredCubemap — roughness-convolved specular IBL environment.
//
// Ported from sampo's src/rendering/prefiltered_cubemap.{hpp,cpp} (namespace
// sampo -> badlands) but with a RENDER-PATH prefilter instead of sampo's
// compute/storage path: badlands' trimmed GpuContext never enabled storage
// textures, and RGBA16Float storage-write needs a device feature it lacks.
// RGBA16Float IS renderable, so Generate() renders a fullscreen triangle
// (shaders/ibl/prefilter_render.wesl, GGX importance sample) into each cube
// face + mip level view — mirroring texture_loader.cpp's per-mip render loop.
//
// Each mip = one roughness (roughness = mip/(mips-1)): mip 0 = mirror,
// last mip = fully rough. Sampled in deferred_lighting at roughness*(mips-1).
#include <cstdint>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuPipelineGenerator;

class PrefilteredCubemap {
 public:
  PrefilteredCubemap() = default;

  PrefilteredCubemap(const PrefilteredCubemap&) = delete;
  PrefilteredCubemap& operator=(const PrefilteredCubemap&) = delete;
  PrefilteredCubemap(PrefilteredCubemap&&) = default;
  PrefilteredCubemap& operator=(PrefilteredCubemap&&) = default;

  // (Re)generates the prefiltered cube from a source environment cube view +
  // its filtering sampler. Safe to call repeatedly (Dawn textures are
  // ref-counted; the previous cube is released). Returns false on failure.
  bool Generate(wgpu::Device device, wgpu::Queue queue,
                GpuPipelineGenerator& pipeline_gen,
                wgpu::TextureView source_cube_view,
                wgpu::Sampler source_sampler, uint32_t face_size = 128,
                uint32_t mip_count = 5);

  wgpu::TextureView GetView() const { return view_; }  // Cube view, all mips
  wgpu::Sampler GetSampler() const { return sampler_; }
  bool IsValid() const { return texture_ != nullptr; }
  uint32_t GetFaceSize() const { return face_size_; }
  uint32_t GetMipCount() const { return mip_count_; }

  static constexpr wgpu::TextureFormat kFormat =
      wgpu::TextureFormat::RGBA16Float;

 private:
  wgpu::Texture texture_;
  wgpu::TextureView view_;
  wgpu::Sampler sampler_;  // Trilinear (mip interpolation across roughness)
  uint32_t face_size_ = 0;
  uint32_t mip_count_ = 0;
};

}  // namespace badlands
