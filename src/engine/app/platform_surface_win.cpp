#include "engine/app/platform_surface.hpp"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

namespace badlands::rhi_app {

// THIS ARM COMPILES AND REFUSES, and that is the honest state of it.
//
// Getting the HWND is the easy half and is written below. The half that does
// not exist is BackendKind::Dx12 -- CreateDevice has no DX12 arm to return, so
// an app that got a valid surface here would go on to fail at device creation
// with a message about the backend rather than about the port.
//
// Refusing at the surface instead says what is actually missing, once, at the
// first point that can know. A plausible-looking surface handed to a device
// that cannot use it is the "detect and continue" shape rule 3 forbids, one
// layer up.
//
// Nothing here has been run. It goes in the RHI's "tests owed when DX12 lands"
// table rather than being claimed as covered.
NativeSurface CreateNativeSurface(SDL_Window* window) {
  NativeSurface out;
  if (!window) {
    spdlog::error("rhi_app: CreateNativeSurface was given no window");
    return out;
  }
  void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                      nullptr);
  if (!hwnd) {
    spdlog::error("rhi_app: no Win32 HWND on this window: {}", SDL_GetError());
    return out;
  }
  spdlog::error(
      "rhi_app: the Windows surface is not usable yet -- an HWND is available, "
      "but the RHI has no DX12 backend to present through, so there is nothing "
      "to hand it to. Refusing here rather than at device creation, because "
      "this is the first point that knows.");
  return out;  // deliberately invalid; see above
}

void DestroyNativeSurface(NativeSurface& surface) {
  // Nothing is allocated by the arm above. When DX12 lands and it starts
  // returning a real surface, this stays empty: the HWND belongs to SDL and the
  // window destroys it.
  surface.platform_object = nullptr;
  surface.handle = nullptr;
}

rhi::BackendKind NativeBackend() {
  // There is no Dx12 enumerator yet. Naming Metal here would be a lie that
  // compiles; Null at least fails visibly and runs headless.
  return rhi::BackendKind::Null;
}

// Empty, and that emptiness is the point: macOS needs a per-frame autorelease
// pool and Windows needs nothing, so the loop above carries no #if.
PlatformFrameScope::PlatformFrameScope() = default;
PlatformFrameScope::~PlatformFrameScope() = default;

}  // namespace badlands::rhi_app
