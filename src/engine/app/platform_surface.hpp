#pragma once

// The three things that are genuinely per-OS, and nothing else.
//
// SDL3 already provides the window, the events and the input on both macOS and
// Windows, so this is NOT a windowing abstraction. What SDL cannot hand over
// portably is a NATIVE SURFACE for the graphics API, and what has no portable
// spelling at all is macOS's per-frame autorelease pool. Those two, plus which
// RHI backend to ask for, are the whole seam.
//
// Deliberately narrow. A framework here would be generality bought on
// speculation; three functions are what a second platform actually needs, and
// retrofitting even those through several ported apps is the cost this exists
// to avoid.
//
// The WINDOWS arm compiles and REFUSES. There is no DX12 backend yet, so
// CreateNativeSurface logs what is missing and returns failure rather than
// producing something plausible that renders nothing.

#include <cstdint>
#include <string>

#include "engine/rhi/rhi_device.hpp"

struct SDL_Window;

namespace badlands::rhi_app {

// A native surface handle plus whatever the platform needs to keep alive
// alongside it. `handle` is what IRhiDevice::CreateSwapchain takes as its
// `native_window`: a CAMetalLayer* on macOS, an HWND on Windows.
struct NativeSurface {
  void* handle = nullptr;
  // Opaque platform baggage -- an SDL_MetalView on macOS, nothing on Windows.
  // Held so Destroy can release it in the right order: the swapchain owns the
  // layer this view owns.
  void* platform_object = nullptr;

  bool Valid() const { return handle != nullptr; }
};

// Creates the native surface for `window`. Returns an invalid surface, after
// logging, if the platform cannot provide one.
NativeSurface CreateNativeSurface(SDL_Window* window);

// Releases whatever CreateNativeSurface allocated. Safe on an invalid surface.
// MUST run after the swapchain is destroyed: on macOS the swapchain holds the
// layer this owns.
void DestroyNativeSurface(NativeSurface& surface);

// The RHI backend this platform presents through.
rhi::BackendKind NativeBackend();

// One frame's platform-scoped resources.
//
// On macOS this is an autorelease pool, and it is not optional: Metal hands
// back autoreleased objects every frame -- nextDrawable and each command buffer
// -- and without a pool per frame they accumulate for the life of the run and
// look exactly like a GPU memory leak. On Windows it is empty, and the empty
// version is why the loop above needs no #if.
class PlatformFrameScope {
 public:
  PlatformFrameScope();
  ~PlatformFrameScope();

  PlatformFrameScope(const PlatformFrameScope&) = delete;
  PlatformFrameScope& operator=(const PlatformFrameScope&) = delete;

 private:
  void* pool_ = nullptr;
};

}  // namespace badlands::rhi_app
