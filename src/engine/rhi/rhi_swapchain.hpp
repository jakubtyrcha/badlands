#pragma once

// Presentation: acquire a backbuffer, render into it, present it.
//
// Acquisition FAILING IS NORMAL. A minimized window, an occluded one, a
// display change, a drawable pool momentarily exhausted -- all of these mean
// "skip this frame and try the next", not "something is broken". Signalling
// that with a null return would make it indistinguishable from a real failure,
// so the two are separate statuses (rule 5).
//
// Note what is NOT here: frames_in_flight. It lives on DeviceDesc, because the
// frame model works headless and the swapchain must not carry a second copy
// that can disagree with it. A swapchain reads it back from the device.

#include <cstdint>
#include <memory>
#include <string>

#include "engine/rhi/rhi_resources.hpp"
#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

enum class AcquireStatus : uint8_t {
  Ok,    // render into `view`
  Skip,  // transient -- minimized, occluded, timed out, size mismatch
  Lost,  // the surface must be recreated before anything else will work
};

const char* ToString(AcquireStatus s);

struct AcquiredFrame {
  AcquireStatus status = AcquireStatus::Skip;
  ITextureView* view = nullptr;  // non-null exactly when status == Ok
};

struct SwapchainDesc {
  // Platform handle: a CAMetalLayer* or NSWindow* on macOS, an HWND on
  // Windows. Null asks for a headless swapchain, which is what makes the
  // acquire/present state machine testable with no display.
  void* native_window = nullptr;

  // PIXELS, not points. On a HiDPI display these differ by the backing scale,
  // and using points yields a plausible-looking half-resolution image rather
  // than an error.
  uint32_t width = 0;
  uint32_t height = 0;

  Format format = Format::BGRA8Unorm;
  bool vsync = true;
  std::string label;
};

class ISwapchain {
 public:
  virtual ~ISwapchain() = default;

  // The swapchain this one wraps, or null. Only the validation decorator
  // returns non-null.
  //
  // Same reason IRhiDevice::Inner() exists, and the third time a decorator has
  // hidden a backend-specific test hook: a dynamic_cast to the concrete type
  // fails on a wrapped object, so the hook silently does nothing and the test
  // asserts against unchanged state. A check that quietly does not run is
  // worse than one that fails.
  virtual ISwapchain* Inner() { return nullptr; }

  // Takes the backbuffer for the current frame. Call once per frame, and as
  // LATE as possible: holding a drawable stalls the pipeline, and the CPU is
  // already paced by IRhiDevice::BeginFrame rather than by this.
  virtual AcquiredFrame Acquire() = 0;

  // Hands the acquired backbuffer to the display. Exactly one Present per
  // successful Acquire.
  virtual void Present() = 0;

  // Resizes to `width` x `height` PIXELS. Views handed out before this call
  // are stale; anything sized to match the swapchain must be recreated.
  //
  // Call at ONE point in the frame -- after the device's BeginFrame and before
  // any acquire or encoding. Old targets are Destroy()ed rather than waited
  // on: deferred deletion keeps them alive for frames still in flight, which
  // is why a live resize does not need WaitIdle and does not hitch.
  virtual void Resize(uint32_t width, uint32_t height) = 0;

  virtual uint32_t GetWidth() const = 0;
  virtual uint32_t GetHeight() const = 0;
  virtual Format GetFormat() const = 0;
};

using SwapchainPtr = std::unique_ptr<ISwapchain>;

}  // namespace badlands::rhi
