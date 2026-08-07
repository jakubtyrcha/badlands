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

// How the compositor must interpret what is presented.
//
// This is NOT decoration on top of the format. The format says how many bits a
// channel has; this says what those bits MEAN -- which primaries they are in and
// whether they carry a transfer curve. A surface whose values leave [0,1] has no
// defined meaning at all without it, which is why ValidateSwapchainDesc refuses
// that combination rather than presenting something the compositor must guess at.
enum class ColorSpace : uint8_t {
  // Untagged. Values are sRGB-encoded and in sRGB primaries -- what every
  // existing call site has always meant, so it stays the default.
  Srgb,
  // Display P3 primaries, sRGB transfer curve. P3 deliberately reuses that
  // curve, so an 8-bit surface changes gamut without changing encoding.
  DisplayP3,
  // Display P3 primaries, LINEAR, unbounded. The EDR path: values above 1.0
  // reach the compositor as headroom instead of clipping at SDR white.
  ExtendedLinearDisplayP3,
};

const char* ToString(ColorSpace s);

// The shader-side output mode IS this enum, passed through as a uint --
// "which colour space am I writing for" is one question, so it must not become
// two values that can disagree (rule 5). shaders/slang/common/output_transform.slang
// mirrors these numbers; the asserts make a reorder a compile error here rather
// than a wrong image there.
static_assert(uint8_t(ColorSpace::Srgb) == 0);
static_assert(uint8_t(ColorSpace::DisplayP3) == 1);
static_assert(uint8_t(ColorSpace::ExtendedLinearDisplayP3) == 2);

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
  // Default Srgb, so every call site written before this existed keeps its
  // exact behaviour: untagged, which is what they already got.
  ColorSpace color_space = ColorSpace::Srgb;
  bool vsync = true;
  std::string label;
};

// Refuses a desc that cannot be presented meaningfully. Shared rather than
// per-backend so Metal and Null cannot disagree about what is constructible
// (rule 13), and returns false AFTER logging what it refused and why.
//
// The one refusal today: an extended-range surface on a real window with no
// colour space. Linear values into an untagged layer have no defined transfer,
// so the compositor applies whatever it assumes -- an image that is wrong in a
// way that looks like a grading choice. Headless is exempt: nothing is
// presented, so there is no transfer to define.
bool ValidateSwapchainDesc(const SwapchainDesc& desc);

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

  // What the swapchain ACTUALLY presents, which is not always what was asked
  // for: tagging a layer can fail at runtime, and the recovery is to drop to an
  // 8-bit format rather than present untagged linear. A caller that built
  // pipelines against the requested format would then be one silent mismatch
  // away from a validation failure, so it must read both back from here.
  virtual Format GetFormat() const = 0;
  virtual ColorSpace GetColorSpace() const = 0;
};

using SwapchainPtr = std::unique_ptr<ISwapchain>;

}  // namespace badlands::rhi
