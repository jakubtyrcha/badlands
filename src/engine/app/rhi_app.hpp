#pragma once

// The app layer for the RHI era: what SdlViewerApp is to Dawn.
//
// SdlViewerApp cannot serve, and not for want of trying -- it is Dawn all the
// way through, handing views a wgpu::Device via RenderContext. So this is a
// second app layer by necessity, built on rhi_app_shell (which keeps the window
// and the loop) rather than beside it.
//
// WHAT IT OWNS, so an app does not: SDL init, device creation, the Slang
// compiler, the window and swapchain, ImGui in full, argument parsing, frame
// timing, screenshots, and shutdown in the right order. object_viewer's
// RunWindowed was ~350 lines of exactly that, and rhi_lab duplicated most of
// it.
//
// WHAT IT DOES NOT OWN: the scene. There is no scene target here and no
// tonemap -- an app renders whatever it likes into the backbuffer, and the
// layer composites debug UI on top of what it presented. The one exception is
// ImGui's own overlay, which the layer must own because it cannot be correct
// otherwise (see rhi_app.cpp).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "engine/app/rhi_app_shell.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::rhi_app {

// What a view is built against. Everything here exists only after the app has
// started, which is why views arrive through a factory.
struct RhiAppContext {
  rhi::IRhiDevice* device = nullptr;
  slang::SlangCompiler* compiler = nullptr;
  // READ BACK from the swapchain, never assumed: the shell may present
  // RGBA16Float on an HDR display, and may drop off it again if the layer
  // refuses to tag. A pipeline built against a guess is a pipeline built for an
  // attachment that does not exist.
  rhi::Format surface_format = rhi::Format::Undefined;
  rhi::ColorSpace surface_color_space = rhi::ColorSpace::Srgb;
  uint32_t width = 0;   // PIXELS
  uint32_t height = 0;  // PIXELS
};

// How far time has moved, and what drove it.
//
// THE LAYER DEALS ONLY IN REAL AND PRESENTATION TIME. It never produces a tick:
// `fixed_steps` is a COUNT of elapsed fixed steps and the view maps those onto
// its own clock. Producing ticks here would put the simulation's time base in
// the app shell, which the four-time-bases contract forbids.
struct FrameTime {
  float real_dt = 0.0f;         // wall seconds, or exactly the fixed step
  double elapsed = 0.0;         // since the first frame
  uint32_t fixed_steps = 0;     // whole fixed steps elapsed this frame
  uint64_t index = 0;           // frames begun, from 0
  const bool* keys = nullptr;   // SDL_GetKeyboardState, valid for this call
};

// One application view. The RHI-era counterpart of AppView.
//
// Deliberately carries NO entt::registry, no SceneContext and no Dawn type.
// AppView carries all three; the registry belongs to the simulation and
// SceneContext is the Dawn renderer's. A view that needs a registry owns one.
class RhiAppView {
 public:
  virtual ~RhiAppView() = default;

  // Build pipelines and targets here. Returns false, after logging, to abort
  // the run -- rendering an empty scene instead would be a silent failure.
  virtual bool Initialize(const RhiAppContext& ctx) = 0;

  // One SDL event, AFTER ImGui has had first refusal. Return true to consume.
  virtual bool OnEvent(const SDL_Event& event) { (void)event; return false; }

  // The surface changed size; the swapchain has already been resized. Return
  // false to stop the loop: a target rebuild that failed leaves nothing sane.
  virtual bool OnResize(uint32_t width, uint32_t height) {
    (void)width; (void)height; return true;
  }

  // After IRhiDevice::BeginFrame. Recycle per-frame allocators HERE, not in
  // Render -- a SKIPPED frame still consumes its slot.
  virtual void OnFrameBegin(uint64_t frame_index) { (void)frame_index; }

  // Camera and simulation.
  virtual void Update(const FrameTime& time) { (void)time; }

  // Per-frame ImGui windows. THE DEBUG UI SURFACE, distinct from any in-world
  // game UI. The layer runs the context and the pass; this only declares
  // windows.
  virtual void DrawUI() {}

  // Record into the acquired backbuffer. Return false to skip presenting this
  // frame; the frame still ends, so pacing stays sound.
  virtual bool Render(rhi::ITextureView* target, const FrameInfo& info) = 0;
};

struct RhiAppConfig {
  std::string title = "badlands";
  uint32_t width = 1280;   // POINTS, as SDL takes
  uint32_t height = 720;
  rhi::ColorSpace color_space = rhi::ColorSpace::Srgb;
  bool prefer_hdr = false;
  bool vsync = true;
  // Search paths for the Slang compiler, in order.
  std::vector<std::string> shader_paths;
};

// Flags the layer parses out of argv, so every app spells them the same way.
struct RhiAppOptions {
  uint64_t max_frames = 0;       // --frames N; 0 = until quit
  float fixed_dt = 0.0f;         // --fixed-dt D; 0 = wall clock
  std::string screenshot_path;   // --screenshot PATH
  bool valid = true;
};

// Parses the layer's own flags and REMOVES them from argv, leaving the rest for
// the app. Exposed for its own test: a flag that is parsed but not removed
// reaches the app's parser as an unknown argument and gets refused there, which
// reads as the app rejecting a flag the layer advertises.
RhiAppOptions ParseAppArgs(int& argc, char** argv);

class RhiApp {
 public:
  using ViewFactory = std::function<std::unique_ptr<RhiAppView>(
      const RhiAppContext&)>;

  explicit RhiApp(RhiAppConfig config) : config_(std::move(config)) {}

  // Runs to completion. Returns a process exit code. Parses and STRIPS the
  // layer's own flags from argv first.
  int Run(int argc, char** argv, const ViewFactory& factory);

  // The same, with the flags already parsed.
  //
  // EXISTS BECAUSE ORDER MATTERS. An app with its own parser must strip the
  // layer's flags BEFORE running it, or --frames reaches the app as an unknown
  // argument and gets refused -- the app rejecting a flag the layer documents.
  // So the app calls ParseAppArgs itself, up front, and hands the result here
  // rather than having argv parsed twice.
  int RunParsed(const RhiAppOptions& options, const ViewFactory& factory);

 private:
  RhiAppConfig config_;
};

}  // namespace badlands::rhi_app
