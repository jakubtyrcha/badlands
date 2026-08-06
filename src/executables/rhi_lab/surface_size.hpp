#pragma once

// What must be rebuilt when the window's pixel size changes.
//
// Pulled out of the render loop as a pure decision so it can be tested without
// a window, and because the loop got it WRONG in a way no windowed test would
// have caught quickly: it gated the resize on the size of the render targets,
// which are deliberately NOT rebuilt at zero size. Minimize, and the targets
// keep the old size; restore to that same size, and the loop sees "nothing
// changed" and never tells the swapchain to come back from 0x0. The window
// stays black until it is dragged to some other size.
//
// The fix is to remember what was last APPLIED TO THE SWAPCHAIN, which is the
// thing whose state actually differs.

#include <cstdint>

namespace badlands::rhi_lab {

struct ResizeAction {
  // Tell the swapchain about the new size.
  bool resize_swapchain = false;
  // Rebuild the render targets and the tables that reference them. False at
  // zero size: a 0x0 texture is not a thing, and the frame will be skipped.
  bool rebuild_targets = false;
};

class SurfaceSizeTracker {
 public:
  SurfaceSizeTracker(uint32_t width, uint32_t height)
      : applied_w_(width), applied_h_(height) {}

  // Called once per frame with the latest pixel size the window has reported.
  ResizeAction Update(uint32_t pending_w, uint32_t pending_h) {
    if (pending_w == applied_w_ && pending_h == applied_h_) return {};
    applied_w_ = pending_w;
    applied_h_ = pending_h;
    return {.resize_swapchain = true,
            .rebuild_targets = pending_w > 0 && pending_h > 0};
  }

  uint32_t width() const { return applied_w_; }
  uint32_t height() const { return applied_h_; }

 private:
  uint32_t applied_w_ = 0;
  uint32_t applied_h_ = 0;
};

}  // namespace badlands::rhi_lab
