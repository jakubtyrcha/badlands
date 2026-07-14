#pragma once

// Task S2.A3: app-framework frame-sequence screenshot recorder. Captures
// rendered frames to a numbered PNG sequence via the offscreen readback path
// (same as SaveScreenshot). Modest debug feature -- re-renders the view
// offscreen per captured frame; not real-time video.

#include <cstdint>
#include <string>

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
  // (w,h) and writes the next numbered PNG. No-op if inactive.
  void CaptureFrame(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t w, uint32_t h);

 private:
  bool active_ = false;
  std::string dir_;
  uint32_t frame_index_ = 0;
};

}  // namespace badlands
