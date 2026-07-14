#include "engine/app/screenshot_recorder.hpp"

#include <cstdio>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "engine/app/screenshot.hpp"

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

void ScreenshotRecorder::CaptureFrame(GpuContext& gpu,
                                      GpuPipelineGenerator& pipeline_gen,
                                      AppView& view, uint32_t w, uint32_t h) {
  if (!active_) return;

  char name[32];
  std::snprintf(name, sizeof(name), "frame_%05u.png", frame_index_);
  std::filesystem::path path = std::filesystem::path(dir_) / name;

  if (SaveScreenshot(gpu, pipeline_gen, view, w, h, path.string())) {
    ++frame_index_;
  } else {
    spdlog::error("ScreenshotRecorder::CaptureFrame: failed to write {}",
                  path.string());
  }
}

}  // namespace badlands
