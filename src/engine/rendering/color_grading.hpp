#pragma once

// ColorGrading — Oklab stylization pass (Task: P3/HDR color grading). A
// fullscreen HDR->HDR remap on the linear-sRGB working buffer: crush blacks,
// desaturate midtones sparing saturated colors and HDR highlights. Owned by
// SceneRenderer and invoked after projected decals, before debug lines, via
// the renderer's snapshot-copy pattern (the renderer copies the HDR colour
// into its snapshot texture and hands both views here — a target can't be
// sampled while bound).
//
// Bespoke pass wired directly to GpuPipelineGenerator (mirrors the
// contact-shadows / tonemap passes, and VolumetricFog's config ownership).
// Game-agnostic; disabled by default (ColorGradingConfig.enabled).
#include <cstdint>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/color_grading_config.hpp"

namespace badlands {

class GpuPipelineGenerator;
class FrameContext;
class GpuTimer;

class ColorGrading {
 public:
  // gen: non-owning pipeline cache (must outlive this). hdr_format: the HDR
  // working-buffer format the pass renders into (== the renderer's
  // accumulation format).
  void Initialize(GpuPipelineGenerator* gen, wgpu::TextureFormat hdr_format);

  void SetConfig(const ColorGradingConfig& config) { config_ = config; }
  const ColorGradingConfig& GetConfig() const { return config_; }
  ColorGradingConfig& MutableConfig() { return config_; }  // ImGui editor

  // Records the fullscreen grade from `source` (the HDR snapshot) into
  // `target` (the HDR colour view) onto `frame`. The caller is responsible
  // for the snapshot copy and for gating on GetConfig().enabled. No-op when
  // uninitialized or on pipeline-compile failure (logged once).
  void Render(FrameContext& frame, GpuTimer& gpu_timer,
              wgpu::TextureView source, wgpu::TextureView target);

 private:
  GpuPipelineGenerator* pipeline_generator_ = nullptr;
  wgpu::TextureFormat hdr_format_ = wgpu::TextureFormat::RGBA16Float;
  ColorGradingConfig config_;
  bool logged_compile_error_ = false;
};

}  // namespace badlands
