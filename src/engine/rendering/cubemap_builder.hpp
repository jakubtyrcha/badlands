#pragma once

// CubemapBuilder — builds a standard (NOT EAC) RGBA16Float cubemap on the CPU
// by evaluating a radiance function at each face texel's world-space direction.
//
// This is the source-environment producer for IBL: PrefilteredCubemap reads
// the resulting cube view + a filtering sampler and GGX-convolves it into the
// prefiltered specular chain. mip0 only (the prefilter samples mip0 anyway).
//
// Face-texel -> direction uses the standard cube-map major-axis convention
// shared by WebGPU/D3D/Vulkan/Metal and by shaders/ibl/prefilter_render.wesl's
// faceUVToDirection(), so hardware cube sampling of the built cube returns the
// value the radiance function produced for that direction.
#include <cstdint>
#include <functional>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

namespace badlands {

class CubemapBuilder {
 public:
  CubemapBuilder() = default;

  // Radiance function: world-space unit direction -> RGBA (linear HDR). Alpha
  // is stored but unused by the IBL pipeline.
  using RadianceFn = std::function<glm::vec4(glm::vec3 dir)>;

  // Builds an RGBA16Float cube (face_size x face_size x 6, 1 mip). Returns
  // false and leaves this invalid on allocation failure. Safe to call again to
  // rebuild (Dawn textures are ref-counted; the previous one is released).
  bool Build(wgpu::Device device, wgpu::Queue queue, uint32_t face_size,
             const RadianceFn& fn);

  wgpu::TextureView GetView() const { return view_; }
  bool IsValid() const { return texture_ != nullptr; }
  uint32_t GetFaceSize() const { return face_size_; }

  // Standard cube-map face-index + UV([0,1], v=0 top) -> unit direction.
  // Exposed so callers (and tests) can share the exact convention.
  static glm::vec3 FaceUVToDirection(uint32_t face, float u, float v);

 private:
  wgpu::Texture texture_;
  wgpu::TextureView view_;
  uint32_t face_size_ = 0;
};

}  // namespace badlands
