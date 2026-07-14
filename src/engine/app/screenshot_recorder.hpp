#pragma once

// Task S2.A3: app-framework frame-sequence screenshot recorder. Captures
// rendered frames to a numbered PNG sequence via the offscreen readback path.
// Modest debug feature -- re-renders the view offscreen per captured frame;
// not real-time video. Unlike the one-shot SaveScreenshot, it OWNS a
// persistent offscreen SceneRenderer + target (BRDF LUT / fallback cube / IBL
// prefilter are built once and reused, not rebuilt per frame).

#include <cstdint>
#include <string>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/scene_renderer.hpp"  // SceneRenderer, GBufferDebugMode

namespace badlands {

class GpuContext;
class GpuPipelineGenerator;
class AppView;

class ScreenshotRecorder {
 public:
  // Begins recording to dir/frame_%05d.png (creates `dir` if missing; resets
  // the frame counter).
  void Start(const std::string& dir);

  void Stop();

  bool active() const { return active_; }

  // Call once per windowed frame while active: renders `view` offscreen at
  // (w,h) with the given G-buffer `debug_mode` and writes the next numbered
  // PNG. The offscreen SceneRenderer + RGBA8Unorm target are created lazily on
  // the first call and reused; they are recreated only if (w,h) changes.
  // No-op if inactive.
  void CaptureFrame(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t w, uint32_t h,
                    GBufferDebugMode debug_mode);

 private:
  // Ensures offscreen_renderer_ + offscreen_texture_/view_ exist and are sized
  // (w,h). Returns false (after logging) on allocation failure.
  bool EnsureTarget(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    uint32_t w, uint32_t h);

  bool active_ = false;
  std::string dir_;
  uint32_t frame_index_ = 0;

  // Persistent offscreen render resources (lazily initialized on first
  // CaptureFrame; reused across frames, recreated only on a size change).
  SceneRenderer offscreen_renderer_;
  bool renderer_ready_ = false;
  wgpu::Texture offscreen_texture_;
  wgpu::TextureView offscreen_view_;
  uint32_t tex_width_ = 0;
  uint32_t tex_height_ = 0;
};

}  // namespace badlands
