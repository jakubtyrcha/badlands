#include "engine/app/rhi_app_shell.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "engine/app/platform_surface.hpp"
#include "engine/app/surface_size.hpp"

namespace badlands::rhi_app {

using namespace badlands::rhi;

std::unique_ptr<AppShell> AppShell::Create(IRhiDevice& device,
                                           const AppShellDesc& desc) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    spdlog::error("rhi_app: SDL_Init failed: {}", SDL_GetError());
    return nullptr;
  }
  // Without this, a click that gives the window focus is swallowed rather than
  // delivered -- the same macOS input problem as the raise in Run().
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

  auto shell = std::unique_ptr<AppShell>(new AppShell());
  shell->device_ = &device;
  shell->window_ = SDL_CreateWindow(
      desc.title.c_str(), int(desc.width), int(desc.height),
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!shell->window_) {
    spdlog::error("rhi_app: SDL_CreateWindow failed: {}", SDL_GetError());
    return nullptr;
  }

  // THE ONLY PER-OS STEP in this file. Everything below -- the resize
  // coalescing, the focus dance, the pacing -- is the same on both targets,
  // which is why the seam is three functions rather than a framework.
  shell->surface_ = CreateNativeSurface(shell->window_);
  if (!shell->surface_.Valid()) return nullptr;  // it logged why

  // PIXELS, not points. On a HiDPI display these differ by the backing scale,
  // and using points renders at half resolution into a full-size drawable --
  // which looks plausible rather than wrong.
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(shell->window_, &pw, &ph);
  shell->applied_width_ = uint32_t(std::max(0, pw));
  shell->applied_height_ = uint32_t(std::max(0, ph));

  // HDR is a property of the DISPLAY, not of the app, so it can only be
  // decided after the window exists. SDL reports it per-window; the headroom is
  // read only to log it, because the decision is binary.
  Format format = desc.present_format;
  ColorSpace color_space = desc.color_space;
  if (desc.prefer_hdr) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(shell->window_);
    if (SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,
                               false)) {
      format = Format::RGBA16Float;
      color_space = ColorSpace::ExtendedLinearDisplayP3;
      spdlog::info("rhi_app: HDR display (headroom {:.2f}), presenting {} / {}",
                   SDL_GetFloatProperty(props,
                                        SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT,
                                        1.0f),
                   ToString(format), ToString(color_space));
    }
  }

  shell->swapchain_ = device.CreateSwapchain(
      {.native_window = shell->surface_.handle,
       .width = shell->applied_width_,
       .height = shell->applied_height_,
       .format = format,
       .color_space = color_space,
       .vsync = desc.vsync,
       .label = desc.title});
  if (!shell->swapchain_) return nullptr;  // CreateSwapchain logged why

  // Read BACK rather than assumed. Tagging can fail, in which case the
  // swapchain has already abandoned the float format -- and a caller that
  // built pipelines against what it asked for would be one silent mismatch
  // from a validation failure.
  if (shell->swapchain_->GetFormat() != format ||
      shell->swapchain_->GetColorSpace() != color_space) {
    spdlog::warn("rhi_app: asked for {} / {}, presenting {} / {}",
                 ToString(format), ToString(color_space),
                 ToString(shell->swapchain_->GetFormat()),
                 ToString(shell->swapchain_->GetColorSpace()));
  }
  return shell;
}

Format AppShell::SurfaceFormat() const {
  return swapchain_ ? swapchain_->GetFormat() : Format::Undefined;
}

ColorSpace AppShell::SurfaceColorSpace() const {
  return swapchain_ ? swapchain_->GetColorSpace() : ColorSpace::Srgb;
}

AppShell::~AppShell() {
  // Order matters: the swapchain holds the layer the native surface owns.
  swapchain_.reset();
  DestroyNativeSurface(surface_);
  if (window_) SDL_DestroyWindow(window_);
  SDL_Quit();
}

void AppShell::RequestResizePoints(uint32_t width, uint32_t height) {
  if (window_) SDL_SetWindowSize(window_, int(width), int(height));
}

FrameOutcome RunOneFrame(IRhiDevice& device, ISwapchain& swapchain,
                         const AppShellCallbacks& cb, FrameInfo& info,
                         const std::function<bool(FrameInfo&)>& apply_resize) {
  FrameOutcome outcome;

  // Paced HERE, at the top of the frame, not at Acquire. Blocking at acquire
  // would stall the CPU after input has been sampled and the whole update has
  // run, which is the difference between one frame of latency and three.
  device.BeginFrame();
  if (cb.OnFrameBegin) cb.OnFrameBegin(device.CurrentFrame());

  // RESIZE RULE 1: applied at exactly one point, after the pacing wait and
  // before any acquire or encoding. Nothing is recreated mid-frame.
  const bool ready = !apply_resize || apply_resize(info);

  AcquiredFrame frame;  // defaults to Skip
  if (ready) frame = swapchain.Acquire();
  if (frame.status == AcquireStatus::Ok) {
    if (cb.OnRender && cb.OnRender(frame.view, info)) {
      swapchain.Present();
      outcome.presented = true;
    } else {
      // NOT a skipped frame -- the run ends. The drawable stays acquired and
      // every later Acquire would be refused, so continuing means a black
      // window for the rest of the process. See AppShellCallbacks::OnRender.
      spdlog::error("rhi_app: frame {} could not be recorded, stopping",
                    info.index);
      outcome.aborted = true;
    }
  } else if (frame.status == AcquireStatus::Lost) {
    spdlog::warn("rhi_app: surface lost, recreating");
    swapchain.Resize(0, 0);
    swapchain.Resize(info.width, info.height);
  }
  // Skip needs no handling at all: EndFrame retires a frame that submitted
  // nothing, which is what keeps a minimized window from exhausting the pacing
  // budget.
  //
  // And the abort is REPORTED rather than thrown from here, so this always
  // runs: a break between BeginFrame and EndFrame takes a semaphore count that
  // is never returned, and the destructor then trips libdispatch's "deallocated
  // while in use" trap -- a crash that points nowhere near its cause.
  device.EndFrame();
  if (!ready) outcome.aborted = true;
  return outcome;
}

RunStats AppShell::Run(const AppShellCallbacks& cb, uint64_t max_frames) {
  RunStats stats;
  // Coalesced: a live drag streams size events, and recreating the layer per
  // event fights the frame in flight. One recreate per frame, at a defined
  // point.
  uint32_t pending_w = applied_width_;
  uint32_t pending_h = applied_height_;
  SurfaceSizeTracker surface{applied_width_, applied_height_};

  // Built ONCE, not per frame: it captures by reference and runs inside the
  // frame, between the pacing wait and the acquire.
  //
  // Gated on what was last applied TO THE SWAPCHAIN, not on the targets: the
  // targets are deliberately not rebuilt at zero size, so gating on them made a
  // minimize-then-restore-to-the-same-size never tell the swapchain to come
  // back from 0x0.
  const std::function<bool(FrameInfo&)> apply_resize =
      [&](FrameInfo& info) -> bool {
    if (const auto action = surface.Update(pending_w, pending_h);
        action.resize_swapchain) {
      swapchain_->Resize(pending_w, pending_h);
      if (action.rebuild_targets) {
        applied_width_ = pending_w;
        applied_height_ = pending_h;
        if (cb.OnResize && !cb.OnResize(applied_width_, applied_height_)) {
          spdlog::error("rhi_app: could not rebuild for {}x{}, stopping",
                        pending_w, pending_h);
          return false;
        }
      }
    }
    // RESIZE RULE 2: the size is captured once and used for everything.
    info.width = applied_width_;
    info.height = applied_height_;
    return true;
  };

  bool raised_on_show = false;
  running_ = true;
  uint64_t last_ticks = SDL_GetPerformanceCounter();
  const double tick_freq = double(SDL_GetPerformanceFrequency());

  while (running_) {
    // Empty on Windows, an autorelease pool on macOS: Metal hands back
    // autoreleased objects every frame -- nextDrawable and each command buffer
    // -- and without a pool per frame they accumulate for the life of the run
    // and look exactly like a GPU memory leak. The emptiness of the other arm
    // is what keeps this loop free of #if.
    PlatformFrameScope frame_scope;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      // QUIT and Escape are handled BEFORE the caller sees them, because they
      // are app lifecycle rather than input. An ImGui pass sets
      // WantCaptureKeyboard the moment a widget takes focus and then consumes
      // every key-down, which swallowed Escape and left the documented "Esc to
      // quit" doing nothing -- a window that can only be closed by its title
      // bar. Everything else still goes to the caller first.
      if (e.type == SDL_EVENT_QUIT ||
          (e.type == SDL_EVENT_KEY_DOWN &&
           e.key.scancode == SDL_SCANCODE_ESCAPE)) {
        running_ = false;
        continue;
      }

      // The caller sees every other event FIRST and may consume it, which is
      // how an ImGui pass takes a click the camera must not also act on.
      if (cb.OnEvent && cb.OnEvent(e)) continue;

      if (e.type == SDL_EVENT_WINDOW_SHOWN ||
          e.type == SDL_EVENT_WINDOW_EXPOSED) {
        // Raised only once the OS reports the window visible. A pre-loop raise
        // is too early on macOS -- Cocoa only activates a visible window -- and
        // without this the app starts without keyboard focus, so WASD does
        // nothing and it reads as a broken camera.
        if (!raised_on_show) {
          SDL_RaiseWindow(window_);
          raised_on_show = true;
        }
      } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int nw = 0, nh = 0;
        SDL_GetWindowSizeInPixels(window_, &nw, &nh);
        pending_w = uint32_t(std::max(0, nw));
        pending_h = uint32_t(std::max(0, nh));
      }
    }

    const uint64_t now = SDL_GetPerformanceCounter();
    const float dt = float(double(now - last_ticks) / tick_freq);
    last_ticks = now;

    FrameInfo info{.index = stats.frames_begun,
                   .dt = dt,
                   .width = applied_width_,
                   .height = applied_height_,
                   .keys = SDL_GetKeyboardState(nullptr)};
    if (cb.OnUpdate) cb.OnUpdate(info);

    // Paced HERE, at the top of the frame, not at Acquire. Blocking at acquire
    // would stall the CPU after input has been sampled and the whole update has
    // run, which is the difference between one frame of latency and three.
    ++stats.frames_begun;
    const FrameOutcome outcome =
        RunOneFrame(*device_, *swapchain_, cb, info, apply_resize);
    if (outcome.presented) ++stats.frames_presented;
    if (outcome.aborted) {
      stats.aborted = true;
      break;
    }
    if (max_frames > 0 && stats.frames_begun >= max_frames) running_ = false;
  }

  device_->WaitIdle();
  int final_pw = 0, final_ph = 0;
  SDL_GetWindowSizeInPixels(window_, &final_pw, &final_ph);
  stats.final_width = uint32_t(std::max(0, final_pw));
  stats.final_height = uint32_t(std::max(0, final_ph));
  if (swapchain_) {
    stats.final_swapchain_width = swapchain_->GetWidth();
    stats.final_swapchain_height = swapchain_->GetHeight();
  }
  return stats;
}

}  // namespace badlands::rhi_app
