#include "engine/app/platform_surface.hpp"

#include <Foundation/Foundation.h>
#include <SDL3/SDL_metal.h>
#include <spdlog/spdlog.h>

namespace badlands::rhi_app {

NativeSurface CreateNativeSurface(SDL_Window* window) {
  NativeSurface out;
  if (!window) {
    spdlog::error("rhi_app: CreateNativeSurface was given no window");
    return out;
  }
  SDL_MetalView view = SDL_Metal_CreateView(window);
  if (!view) {
    spdlog::error("rhi_app: SDL_Metal_CreateView failed: {}", SDL_GetError());
    return out;
  }
  out.platform_object = view;
  out.handle = SDL_Metal_GetLayer(view);
  if (!out.handle) {
    spdlog::error("rhi_app: SDL_Metal_GetLayer returned no layer");
    SDL_Metal_DestroyView(view);
    out.platform_object = nullptr;
  }
  return out;
}

void DestroyNativeSurface(NativeSurface& surface) {
  if (surface.platform_object) {
    SDL_Metal_DestroyView(SDL_MetalView(surface.platform_object));
  }
  surface.platform_object = nullptr;
  surface.handle = nullptr;
}

rhi::BackendKind NativeBackend() { return rhi::BackendKind::Metal; }

// A raw NSAutoreleasePool rather than @autoreleasepool, because the scope is a
// C++ object's lifetime and the block form cannot span one. This is the same
// mechanism metal::AutoreleasePoolScope used, moved here so the loop above has
// no platform in it.
PlatformFrameScope::PlatformFrameScope()
    : pool_([[NSAutoreleasePool alloc] init]) {}

PlatformFrameScope::~PlatformFrameScope() {
  [static_cast<NSAutoreleasePool*>(pool_) drain];
}

}  // namespace badlands::rhi_app
