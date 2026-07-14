#include "engine/app/sdl_viewer_app.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include "engine/app/screenshot.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"

namespace badlands {

SdlViewerApp::SdlViewerApp(SdlViewerConfig config) : config_(std::move(config)) {}

SdlViewerApp::~SdlViewerApp() {
  view_.reset();
  gpu_.Shutdown();
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

int SdlViewerApp::Run(int argc, char** argv, const ViewFactory& factory) {
  std::string screenshot_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    }
  }
  const bool screenshot_mode = !screenshot_path.empty();

  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

  // In screenshot mode we render into an offscreen texture, not the SDL
  // surface, but a window is still needed: GpuContext::Initialize requires
  // one to create the WebGPU surface an adapter is requested as compatible
  // with.
  window_ = SDL_CreateWindow(config_.window_title.c_str(), config_.width,
                             config_.height, SDL_WINDOW_RESIZABLE);
  if (!window_) return 1;

  if (!gpu_.Initialize(window_)) return 1;
  spdlog::info("SdlViewerApp: window surface format = {}",
               static_cast<int>(gpu_.GetSurfaceFormat()));

  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(window_, &width, &height);

  pipeline_gen_ = std::make_unique<GpuPipelineGenerator>(
      gpu_.GetDevice(), FindShaderDirectory());

  renderer_.Initialize(gpu_.GetDevice(), gpu_.GetQueue(), pipeline_gen_.get(),
                       gpu_.GetSurfaceFormat(), static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height));

  RenderContext ctx{gpu_.GetDevice(), gpu_.GetQueue(), pipeline_gen_.get(),
                    &renderer_, gpu_.GetSurfaceFormat()};
  view_ = factory(ctx);
  if (!view_) {
    spdlog::error("SdlViewerApp: view factory returned null");
    return 1;
  }
  view_->Initialize(ctx);
  view_->OnResize(width, height);

  if (screenshot_mode) {
    bool ok = SaveScreenshot(gpu_, *pipeline_gen_, *view_,
                             static_cast<uint32_t>(width),
                             static_cast<uint32_t>(height), screenshot_path);
    return ok ? 0 : 1;
  }

  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  uint64_t last_time = SDL_GetPerformanceCounter();

  bool render_ok_logged = false;
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (width > 0 && height > 0) {
          gpu_.Configure(static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height));
          renderer_.Resize(static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height));
          view_->OnResize(width, height);
        }
      } else {
        view_->HandleEvent(e, width, height);
      }
    }

    const uint64_t current_time = SDL_GetPerformanceCounter();
    const float dt = perf_freq > 0
                         ? static_cast<float>(current_time - last_time) /
                               static_cast<float>(perf_freq)
                         : (1.0f / 60.0f);
    last_time = current_time;

    view_->Update(dt, SDL_GetKeyboardState(nullptr));

    wgpu::TextureView surface = gpu_.AcquireSurfaceTexture();
    if (!surface) continue;

    renderer_.Render(view_->GetCamera(), view_->GetRegistry(),
                     view_->GetSceneContext(), surface);

    gpu_.Present();

    if (!render_ok_logged) {
      render_ok_logged = true;
      if (g_gpu_error_flag) {
        spdlog::error(
            "SdlViewerApp: render FAILED: Dawn validation error(s) occurred");
      } else {
        spdlog::info("SdlViewerApp: render OK");
      }
    }
  }

  return 0;
}

}  // namespace badlands
