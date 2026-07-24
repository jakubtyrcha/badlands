#pragma once

// Task S2.A1: game-agnostic app framework. Trimmed port of sampo's
// apps/src/app-framework/sdl_viewer_app.* (drops weave/DOM/background-task/
// video rendering — none of that is ported to badlands). Owns the SDL
// window + GpuContext + GpuPipelineGenerator + SceneRenderer + the AppView,
// and drives the acquire -> AppView::Update -> SceneRenderer::Render ->
// GpuContext::Present loop (or, in `--screenshot` mode, a single offscreen
// render via screenshot.hpp).

#include <functional>
#include <memory>
#include <string>

#include "engine/app/app_view.hpp"
#include "engine/app/render_context.hpp"
#include "engine/app/screenshot_recorder.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

struct SDL_Window;

namespace badlands {

struct SdlViewerConfig {
  std::string window_title = "badlands";
  int width = 1600;
  int height = 900;
};

class SdlViewerApp {
 public:
  using ViewFactory = std::function<std::unique_ptr<AppView>(const RenderContext&)>;

  explicit SdlViewerApp(SdlViewerConfig config);
  ~SdlViewerApp();

  // Non-copyable, non-movable (owns SDL/GPU resources with no move support).
  SdlViewerApp(const SdlViewerApp&) = delete;
  SdlViewerApp& operator=(const SdlViewerApp&) = delete;
  SdlViewerApp(SdlViewerApp&&) = delete;
  SdlViewerApp& operator=(SdlViewerApp&&) = delete;

  // Runs the app: init SDL/GPU/renderer, build RenderContext, create the view
  // via `factory`, then loop until quit. Returns process exit code.
  // If argv contains `--screenshot <path>`, renders ONE frame to an offscreen
  // PNG (no window loop) and returns.
  int Run(int argc, char** argv, const ViewFactory& factory);

 private:
  // Dear ImGui context + custom Dawn backend init (Task S2.A2). Windowed
  // path only -- screenshot mode never calls this, so ImGui stays
  // uninitialized (and the destructor's shutdown is guarded accordingly).
  void InitImGui(int width, int height);

  // (Re)creates the window-sized RGBA8 UI-overlay texture (game UI + ImGui
  // draw into it in plain sRGB with gamma-space blending; the resolve pass
  // composites it over the scene — see SceneContext::ui_overlay). Called at
  // startup and on pixel-size changes; no-op when the size is unchanged.
  void EnsureUiOverlay(uint32_t width, uint32_t height);

  SdlViewerConfig config_;

  SDL_Window* window_ = nullptr;
  GpuContext gpu_;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen_;
  SceneRenderer renderer_;
  std::unique_ptr<AppView> view_;
  ScreenshotRecorder recorder_;

  // Windowed-path UI overlay target (see EnsureUiOverlay).
  wgpu::Texture ui_overlay_texture_;
  wgpu::TextureView ui_overlay_view_;
  uint32_t ui_overlay_w_ = 0;
  uint32_t ui_overlay_h_ = 0;
};

}  // namespace badlands
