#include "engine/app/screenshot_recorder.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "engine/app/app_view.hpp"
#include "engine/app/screenshot.hpp"
#include "engine/rendering/gpu_context.hpp"

namespace badlands {

void ScreenshotRecorder::Start(const std::string& dir) {
  dir_ = dir;
  frame_index_ = 0;
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    spdlog::error("ScreenshotRecorder::Start: failed to create dir {} ({})",
                  dir_, ec.message());
    active_ = false;
    return;
  }
  active_ = true;
  spdlog::info("ScreenshotRecorder: recording to {}", dir_);
}

void ScreenshotRecorder::Stop() {
  if (active_) {
    spdlog::info("ScreenshotRecorder: stopped after {} frame(s)", frame_index_);
  }
  active_ = false;
}

bool ScreenshotRecorder::EnsureTarget(GpuContext& gpu,
                                      GpuPipelineGenerator& pipeline_gen,
                                      uint32_t w, uint32_t h) {
  if (renderer_ready_ && tex_width_ == w && tex_height_ == h) {
    return true;
  }

  wgpu::Device device = gpu.GetDevice();
  if (!renderer_ready_) {
    // First use: build the persistent offscreen renderer once (BRDF LUT +
    // fallback cube + IBL prefilter machinery live here and are reused).
    offscreen_renderer_.Initialize(device, gpu.GetQueue(), &pipeline_gen,
                                   wgpu::TextureFormat::RGBA8Unorm, w, h,
                                   gpu.HasR8UnormStorage());
    renderer_ready_ = true;
  } else {
    // Size changed: resize the existing renderer's targets in place.
    offscreen_renderer_.Resize(w, h);
  }

  offscreen_texture_ = CreateScreenshotTarget(device, w, h);
  offscreen_view_ = offscreen_texture_.CreateView();
  if (!offscreen_view_) {
    spdlog::error("ScreenshotRecorder: failed to create {}x{} offscreen target",
                  w, h);
    return false;
  }
  tex_width_ = w;
  tex_height_ = h;
  return true;
}

void ScreenshotRecorder::CaptureFrame(GpuContext& gpu,
                                      GpuPipelineGenerator& pipeline_gen,
                                      AppView& view, uint32_t w, uint32_t h,
                                      GBufferDebugMode debug_mode) {
  if (!active_) return;

  if (!EnsureTarget(gpu, pipeline_gen, w, h)) return;

  offscreen_renderer_.SetDebugMode(debug_mode);
  view.Update(0.0f, SDL_GetKeyboardState(nullptr));
  offscreen_renderer_.Render(view.GetCamera(), view.GetRegistry(),
                             view.GetSceneContext(), offscreen_view_);

  char name[32];
  std::snprintf(name, sizeof(name), "frame_%05u.png", frame_index_);
  std::filesystem::path path = std::filesystem::path(dir_) / name;

  if (WriteTextureToPng(gpu.GetInstance(), gpu.GetDevice(), gpu.GetQueue(),
                        offscreen_texture_, w, h, path.string())) {
    ++frame_index_;
  } else {
    spdlog::error("ScreenshotRecorder::CaptureFrame: failed to write {}",
                  path.string());
  }
}

}  // namespace badlands
