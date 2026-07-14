#pragma once

// BRDF Lookup Table for IBL (split-sum approximation).
//
// Ported from sampo's src/rendering/brdf_lut.{hpp,cpp} (namespace sampo ->
// badlands). Same render-path approach as sampo (a fullscreen pass into an
// RG16Float target), but built through badlands' GpuPipelineGenerator
// (explicit-reflection pipeline cache) instead of hand-rolled pipeline
// creation. The shader (shaders/ibl/brdf_lut_render.wesl) has NO bindings.
//
// Layout: RG16Float 2D texture, X = NdotV, Y = roughness,
//   R = scale (multiply with F0), G = bias (add). final = F0 * R + G.
//
// Generated ONCE at renderer init (roughness-only integral, view-independent).
#include <cstdint>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuPipelineGenerator;

class BrdfLut {
 public:
  BrdfLut() = default;

  BrdfLut(const BrdfLut&) = delete;
  BrdfLut& operator=(const BrdfLut&) = delete;
  BrdfLut(BrdfLut&&) = default;
  BrdfLut& operator=(BrdfLut&&) = default;

  // Renders the split-sum LUT into an RG16Float target. pipeline_gen compiles
  // shaders/ibl/brdf_lut_render.wesl. Returns false on failure.
  bool Generate(wgpu::Device device, wgpu::Queue queue,
                GpuPipelineGenerator& pipeline_gen);

  wgpu::TextureView GetView() const { return view_; }
  wgpu::Sampler GetSampler() const { return sampler_; }
  bool IsValid() const { return texture_ != nullptr; }

  static constexpr uint32_t kLutSize = 256;
  static constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RG16Float;

 private:
  wgpu::Texture texture_;
  wgpu::TextureView view_;
  wgpu::Sampler sampler_;  // Bilinear, clamp-to-edge
};

}  // namespace badlands
