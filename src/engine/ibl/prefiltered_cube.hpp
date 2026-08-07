#pragma once

// The roughness-convolved specular environment, and the split-sum LUT beside it.
//
// Mip m holds roughness m/(mips-1); the resolve samples at roughness*(mips-1).
//
// THE TWO PARAMETERIZATIONS ARE ONE FUNCTION AND ITS INVERSE, which is the
// invariant worth stating. It is not that the mapping is linear -- it is that
// whatever the prefilter bakes in, the sampler must ask for. alpha = roughness^2
// belongs inside the GGX distribution (see ibl_sampling.slang), not out here;
// sampling at roughness^2 against a linearly-prefiltered chain reads as
// everything being too sharp, uniformly, with nothing to point at.
//
// Keeping it linear also matches shaders/ibl/prefilter_render.wesl, so the RHI
// and Dawn renderers agree. Changing it is a decision affecting both.

#include <cstdint>
#include <memory>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::ibl {

class PrefilteredCube {
 public:
  static constexpr uint32_t kFaceSize = 128;
  static constexpr uint32_t kMipCount = 5;

  // The roughness baked into `mip`, and the LOD to ask for at a roughness.
  // Exposed as a pair so a test can assert they round-trip, and so the resolve
  // cannot invent its own.
  static float RoughnessForMip(uint32_t mip) {
    return float(mip) / float(kMipCount - 1);
  }
  static float MipForRoughness(float roughness) {
    return roughness * float(kMipCount - 1);
  }

  // Builds the pipeline and the target. Returns null (after logging) on
  // failure. Does NOT convolve anything yet -- there is no source until
  // Generate.
  static std::unique_ptr<PrefilteredCube> Create(rhi::IRhiDevice& device,
                                                 slang::SlangCompiler& compiler);

  // Re-convolves every (face, mip) from `source`, submits, and waits.
  //
  // Synchronous on purpose: this runs at startup and when the environment
  // changes, never inside the frame loop, and a half-built chain sampled by the
  // resolve is worse than a stall nobody sees.
  bool Generate(rhi::ITexture* source_cube, rhi::ISampler* source_sampler);

  rhi::ITexture* Texture() const { return texture_.get(); }
  // The all-faces, all-mips CUBE view the resolve samples.
  rhi::ITextureView* CubeView() const { return cube_view_; }

 private:
  PrefilteredCube() = default;

  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  rhi::TexturePtr texture_;
  rhi::ITextureView* cube_view_ = nullptr;
  rhi::BufferPtr params_;
};

// The split-sum LUT: RG16Float, R = scale for F0, G = bias.
//
// Generated ONCE. The integral is roughness- and angle-parameterized only, with
// no dependence on the environment, so an environment change does not touch it.
class BrdfLut {
 public:
  static constexpr uint32_t kSize = 256;

  // Builds AND generates -- unlike PrefilteredCube there is no input to wait
  // for, so a two-step interface would only create a window in which the object
  // exists and its texture is undefined.
  static std::unique_ptr<BrdfLut> Create(rhi::IRhiDevice& device,
                                         slang::SlangCompiler& compiler);

  rhi::ITexture* Texture() const { return texture_.get(); }
  rhi::ITextureView* View() const { return view_; }

 private:
  BrdfLut() = default;

  rhi::TexturePtr texture_;
  rhi::ITextureView* view_ = nullptr;
};

}  // namespace badlands::ibl
