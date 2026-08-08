#pragma once

// The windowed render loop, on the RHI.
//
// EXTRACTED RATHER THAN COPIED, deliberately. Everything here was learned the
// hard way once already, in rhi_lab:
//
//   * macOS delivers no keyboard focus unless the window is raised AFTER the OS
//     reports it visible, so WASD silently does nothing and reads as a broken
//     camera. A pre-loop raise is too early -- Cocoa only activates a window
//     that is already on screen.
//   * Clicks that give a window focus are swallowed without
//     SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH.
//   * The drawable is sized in PIXELS and SDL_SetWindowSize takes POINTS; on a
//     HiDPI display those differ by the backing scale, and using the wrong one
//     renders at half resolution into a full-size drawable, which looks
//     plausible rather than wrong.
//   * A live resize drag streams size events, so recreating the layer per event
//     fights the frame in flight. One recreate per frame, at a defined point.
//   * Metal hands back autoreleased objects every frame -- nextDrawable and each
//     command buffer -- and without a pool per frame they accumulate for the
//     whole run and look like a GPU memory leak.
//
// A second copy of that list is a second copy of every one of those bugs, fixed
// in one place and not the other.
//
// PLATFORM: the per-OS parts live in platform_surface.hpp -- the native
// surface, the per-frame scope, and the backend choice. Nothing in this file is
// Metal-specific any more, which is what makes "DX12 adds an arm rather than a
// second shell" a fact rather than an intention.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "engine/app/platform_surface.hpp"
#include "engine/rhi/rhi_device.hpp"

namespace badlands::rhi_app {

struct AppShellDesc {
  std::string title = "badlands";
  // Requested in POINTS, because that is what SDL takes. Everything the shell
  // reports back is in PIXELS.
  uint32_t width = 1280;
  uint32_t height = 720;
  // CAMetalLayer accepts BGRA8Unorm and not RGBA8Unorm; the channel order is
  // the hardware's business, so a shader is unchanged either way.
  rhi::Format present_format = rhi::Format::BGRA8Unorm;

  // What to present in when the display is SDR, or when HDR was not asked for
  // or is not available. Defaults to Srgb -- untagged, which is exactly what
  // every caller written before this field got, so adding it changed nobody.
  rhi::ColorSpace color_space = rhi::ColorSpace::Srgb;

  // Ask for an extended-range surface when the DISPLAY reports HDR. Granted,
  // the shell presents RGBA16Float tagged ExtendedLinearDisplayP3 instead of
  // the two fields above.
  //
  // TWO FIELDS RATHER THAN ONE, because they answer different questions: "what
  // gamut do I author in" and "do I want headroom". An app can want P3 without
  // wanting EDR -- object_viewer wants both, rhi_lab wants neither -- and a
  // single flag would force them to move together.
  bool prefer_hdr = false;
  bool vsync = true;
};

// What a frame is being rendered at, and what drove it.
struct FrameInfo {
  uint64_t index = 0;   // frames begun, from 0
  float dt = 0.0f;      // real seconds since the previous frame
  // PIXELS, and the size everything in this frame must use. Captured once per
  // frame: re-reading it mid-frame is what makes half a frame use each size.
  uint32_t width = 0;
  uint32_t height = 0;
  const bool* keys = nullptr;  // SDL_GetKeyboardState, valid for this call only
};

struct AppShellCallbacks {
  // One SDL event, before the shell looks at it. Return true to CONSUME it, so
  // ImGui can take a click the camera should not also act on.
  //
  // NOT called for SDL_EVENT_QUIT or for Escape: those are app lifecycle, and
  // the shell acts on them first. An ImGui pass that consumed every key-down
  // while a widget had focus otherwise made "Esc to quit" stop working.
  std::function<bool(const SDL_Event&)> OnEvent;

  // The surface changed size and the swapchain has already been resized.
  // Return false to stop the loop -- a target rebuild that failed leaves
  // nothing sane to render.
  std::function<bool(uint32_t width, uint32_t height)> OnResize;

  // After events, before the frame is begun. Camera and simulation go here.
  std::function<void(const FrameInfo&)> OnUpdate;

  // Immediately after IRhiDevice::BeginFrame, which has already blocked until a
  // frame slot is free. Recycle per-frame allocators here -- and here rather
  // than in OnRender, because a skipped frame still consumes its slot.
  std::function<void(uint64_t frame_index)> OnFrameBegin;

  // Record into the acquired backbuffer. Returning false ABORTS THE RUN.
  //
  // It used to mean "skip presenting this frame", and that could not work: a
  // swapchain refuses every later Acquire while a drawable is still acquired,
  // and only Present clears it. So one refused frame left the surface wedged
  // for the life of the process -- a black window spamming "acquired twice
  // without a Present" -- while the loop kept counting frames and the process
  // still exited 0. Nothing legitimately skips here anyway: a minimized window
  // is already handled by Acquire returning Skip, before this is called.
  std::function<bool(rhi::ITextureView* target, const FrameInfo&)> OnRender;
};

// How a run ended, and what it did. Exists so a self-test can assert on the
// loop rather than on a screenshot.
struct RunStats {
  uint64_t frames_begun = 0;
  uint64_t frames_presented = 0;
  uint32_t final_width = 0;   // PIXELS
  uint32_t final_height = 0;
  // What the SWAPCHAIN ended up sized for. Reported here rather than fetched
  // afterwards because the shell is gone by then -- an app that reached for it
  // after the run read freed memory and got 0x0, which its own assertion then
  // blamed on the resize.
  uint32_t final_swapchain_width = 0;
  uint32_t final_swapchain_height = 0;
  bool aborted = false;  // a callback asked to stop, or a rebuild failed
};

// What one frame did.
struct FrameOutcome {
  bool presented = false;
  bool aborted = false;  // a rebuild failed, or OnRender refused
};

// ONE FRAME, WITH NO WINDOW IN IT: pace, begin, apply any resize, acquire,
// record, present, end.
//
// EXTRACTED SO THE LOOP CAN BE TESTED. AppShell needs SDL and a real window, so
// the only way to check the claims that matter here -- that a refused render
// does not strand an acquired drawable, that a Skipped acquire still ends its
// frame -- was to open a window and look at it. Both are checkable against the
// Null backend, which has the same swapchain state machine and can be told to
// Skip or Lose on demand.
//
// `apply_resize` runs between the pacing wait and the acquire (RESIZE RULE 1)
// and returns false if the rebuild failed. It is built once by the caller, not
// per frame.
FrameOutcome RunOneFrame(rhi::IRhiDevice& device, rhi::ISwapchain& swapchain,
                         const AppShellCallbacks& callbacks, FrameInfo& info,
                         const std::function<bool(FrameInfo&)>& apply_resize);

class AppShell {
 public:
  // Returns null (after logging) if SDL, the window, the native surface or the
  // swapchain could not be created.
  static std::unique_ptr<AppShell> Create(rhi::IRhiDevice& device,
                                          const AppShellDesc& desc);
  ~AppShell();

  AppShell(const AppShell&) = delete;
  AppShell& operator=(const AppShell&) = delete;

  // Runs until Stop(), a quit event, or Escape. `max_frames` of 0 runs until
  // then; any other value also stops after that many frames, which is how a
  // windowed self-test terminates without a human.
  RunStats Run(const AppShellCallbacks& callbacks, uint64_t max_frames = 0);

  // Ends the loop after the current frame.
  void Stop() { running_ = false; }

  // Asks the window manager for a new size, in POINTS. The new PIXEL size
  // arrives as an event and goes through the same coalescing a user drag uses
  // -- which is the part worth testing.
  void RequestResizePoints(uint32_t width, uint32_t height);

  // PIXELS. What the swapchain and any targets are currently sized for.
  uint32_t Width() const { return applied_width_; }
  uint32_t Height() const { return applied_height_; }

  // What is ACTUALLY being presented. Every pipeline whose colour attachment is
  // the backbuffer must take its format from here rather than from a constant:
  // the shell may pick RGBA16Float over the requested format on an HDR display,
  // and the swapchain may drop back off it again if the layer refuses to tag.
  // A hardcoded BGRA8Unorm beside either of those is a pipeline built for an
  // attachment that no longer exists.
  rhi::Format SurfaceFormat() const;
  // The value shaders take as their output mode. See the static_asserts beside
  // rhi::ColorSpace -- the enum and the shader constant are the same number.
  rhi::ColorSpace SurfaceColorSpace() const;

  rhi::ISwapchain* Swapchain() { return swapchain_.get(); }
  SDL_Window* Window() { return window_; }

 private:
  AppShell() = default;

  rhi::IRhiDevice* device_ = nullptr;
  SDL_Window* window_ = nullptr;
  NativeSurface surface_;
  rhi::SwapchainPtr swapchain_;

  uint32_t applied_width_ = 0;
  uint32_t applied_height_ = 0;
  bool running_ = false;
};

}  // namespace badlands::rhi_app
