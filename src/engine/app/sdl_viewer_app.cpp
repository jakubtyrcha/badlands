#include "engine/app/sdl_viewer_app.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include "core/profiler.hpp"
#include "engine/app/fixed_timestep.hpp"
#include "engine/app/imgui_mouse_input.hpp"
#include "engine/app/screenshot.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/ui/imgui_impl_wgpu_custom.hpp"
#include "engine/ui/ui_renderer.hpp"

namespace badlands {

namespace {
// UI-overlay texture format: 8-bit, sRGB-encoded bytes as the UI shaders
// author them (same convention as the RGBA8 screenshot target). The overlay
// is premultiplied by construction: both UI pipelines blend with
// (srcAlpha, 1-srcAlpha) color / (one, 1-srcAlpha) alpha into the cleared-
// transparent target, which accumulates a premultiplied image.
constexpr wgpu::TextureFormat kUiOverlayFormat = wgpu::TextureFormat::RGBA8Unorm;
}  // namespace

SdlViewerApp::SdlViewerApp(SdlViewerConfig config) : config_(std::move(config)) {}

SdlViewerApp::~SdlViewerApp() {
  view_.reset();
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  gpu_.Shutdown();
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

void SdlViewerApp::InitImGui(int width, int height) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // NOT NavEnableKeyboard: it makes ImGui auto-activate keyboard nav on a panel
  // (navActive => WantCaptureKeyboard true) from startup, which gates the view's
  // WSAD pan off until nav happens to deactivate. The apps don't use ImGui
  // keyboard nav, so leaving it off keeps WantCaptureKeyboard false except when a
  // text field is actually focused.
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGui::StyleColorsDark();

  ImGui_ImplSDL3_InitForOther(window_);

  ImGui_ImplWGPU_InitInfo info = {};
  info.Device = gpu_.GetDevice();
  info.NumFramesInFlight = 3;
  // ImGui draws into the RGBA8 UI-overlay texture (not the surface), in plain
  // sRGB with gamma-space blending — the resolve pass composites the overlay
  // over the scene and owns all P3/EDR conversion. The target format is
  // therefore a constant, independent of the surface.
  info.RenderTargetFormat = kUiOverlayFormat;
  info.DepthStencilFormat = wgpu::TextureFormat::Undefined;  // 2D overlay, no depth
  info.FramebufferWidth = static_cast<uint32_t>(width);
  info.FramebufferHeight = static_cast<uint32_t>(height);
  info.OutputIsLinear = false;  // 8-bit overlay: bytes as authored
  ImGui_ImplWGPU_Init(&info);

  spdlog::info("SdlViewerApp: ImGui initialized");
}

void SdlViewerApp::EnsureUiOverlay(uint32_t width, uint32_t height) {
  if (ui_overlay_view_ && ui_overlay_w_ == width && ui_overlay_h_ == height) {
    return;
  }
  ui_overlay_w_ = width;
  ui_overlay_h_ = height;

  wgpu::TextureDescriptor desc;
  desc.size = {width, height, 1};
  desc.format = kUiOverlayFormat;
  desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  ui_overlay_texture_ = gpu_.GetDevice().CreateTexture(&desc);
  ui_overlay_view_ = ui_overlay_texture_.CreateView();
}

namespace {
// Default recording directory for the F2 live-toggle (no path argument).
constexpr const char* kDefaultRecordDir = "recordings";

// Opt-in (BADLANDS_PROFILE=1) performance logging. The CPU scope-profile tree
// (profiler::ReportToStderr) and the per-pass GPU timings both dump to stderr
// every ~2 s; that noise buries everything else in a normal run. Off by default;
// the profiler is still COMPILED in (BADLANDS_PROFILING), so flipping this env
// var turns it back on with no rebuild. When off, GPU timestamp collection is
// skipped entirely (EnableGpuProfiling is not called), so there's zero cost.
const bool kProfileDump = [] {
  const char* v = std::getenv("BADLANDS_PROFILE");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}();

}  // namespace

int SdlViewerApp::Run(int argc, char** argv, const ViewFactory& factory) {
  std::string screenshot_path;
  std::string record_dir;
  float screenshot_time = 0.5f;  // --time <t01>: time-of-day for --screenshot (noon)
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
      record_dir = argv[++i];
    } else if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
      screenshot_time = static_cast<float>(std::atof(argv[++i]));
    }
  }
  const bool screenshot_mode = !screenshot_path.empty();

  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

  // macOS: a bare (non-.app) binary launched from a shell often opens WITHOUT
  // becoming the active application, so its window is not key. In that state the
  // FIRST click is normally swallowed to activate the window (acceptsFirstMouse
  // defaults NO) instead of being delivered. Turn on click-through so that
  // activating click ALSO reaches the app -- the user's first click isn't wasted.
  // No-op on platforms without the notion. (Read live per-event, but set early.)
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

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
                       static_cast<uint32_t>(height), gpu_.HasR8UnormStorage());
  // Display-P3 resolve (tonemap mode 2) when the surface's CAMetalLayer was
  // tagged P3. NOT set on the headless capture path (SaveScreenshot builds its
  // own renderer) — profile-less PNGs must stay sRGB-referred.
  renderer_.SetOutputIsP3(gpu_.IsP3());
#ifdef BADLANDS_PROFILING
  // Per-pass GPU timing in the live window (prints alongside the CPU profile).
  // Only when BADLANDS_PROFILE is set -- otherwise skip the timestamp-query
  // machinery entirely.
  if (kProfileDump)
    renderer_.EnableGpuProfiling(gpu_.GetInstance(), gpu_.HasTimestampQuery());
#endif

  // ImGui is windowed-path only: screenshot mode renders offscreen via
  // SaveScreenshot() and must stay ImGui-free.
  if (!screenshot_mode) {
    InitImGui(width, height);
  }

  RenderContext ctx{gpu_.GetDevice(), gpu_.GetQueue(), pipeline_gen_.get(),
                    &renderer_, gpu_.GetSurfaceFormat()};
  view_ = factory(ctx);
  if (!view_) {
    spdlog::error("SdlViewerApp: view factory returned null");
    return 1;
  }
  if (!view_->Initialize(ctx)) {
    spdlog::error("SdlViewerApp: view initialization failed");
    return 1;
  }
  view_->OnResize(width, height);

  if (screenshot_mode) {
    // Deterministic capture size: config_ (fixed logical size), NOT the
    // HiDPI window pixel size, which is 2x+ on Retina and would silently
    // change the output resolution. Refresh the view's camera aspect to the
    // capture size before rendering offscreen. renderer_.GetDebugMode() /
    // GetShadowDebugMode() forward the live G-buffer / shadow debug
    // visualization into the capture (SaveScreenshot's renderer is a
    // separate throwaway instance — see its comment).
    const uint32_t shot_w = static_cast<uint32_t>(config_.width);
    const uint32_t shot_h = static_cast<uint32_t>(config_.height);
    view_->OnResize(config_.width, config_.height);
    bool ok = SaveScreenshot(gpu_, *pipeline_gen_, *view_, shot_w, shot_h,
                             screenshot_path, renderer_.GetDebugMode(),
                             renderer_.GetShadowDebugMode(), screenshot_time,
                             renderer_.GetColorGradingConfig());
    return ok ? 0 : 1;
  }

  if (!record_dir.empty()) {
    recorder_.Start(record_dir);
  }

  // Event-based activation: raise the window the first time the OS reports it
  // visible (SDL_EVENT_WINDOW_SHOWN, handled in the loop). A pre-loop raise is
  // too early on macOS -- Cocoa_RaiseWindow only activates once the window is
  // visible -- so we wait for that event instead of polling on a timeout.
  // Combined with SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH (set at startup), input works
  // from the first frames.
  bool raised_on_show = false;
  // Feeds mouse events to ImGui directly (bypassing the stock backend's fragile,
  // wedge-prone mouse path) and owns OS mouse capture during drags. See
  // imgui_mouse_input.
  ImGuiMouseInput mouse_input;

  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  uint64_t last_time = SDL_GetPerformanceCounter();
#ifdef BADLANDS_PROFILING
  double profile_report_accum = 0.0;  // dump the scope profile every ~2 s
#endif

  bool render_ok_logged = false;
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      // Feed mouse events (motion/buttons/leave) to ImGui via our own tracker,
      // NOT the stock backend -- its event path is what wedges on macOS (dropped
      // BUTTON_UP on a foreign windowID, pos invalidated on LEAVE). Everything the
      // tracker consumes it returns true for; forward the rest (wheel, keyboard,
      // text, focus, resize) to the backend. The views still receive mouse events
      // directly below (HandleEvent).
      if (!mouse_input.ProcessEvent(e, ImGui::GetIO())) {
        ImGui_ImplSDL3_ProcessEvent(&e);
      }

      if (e.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (e.type == SDL_EVENT_WINDOW_SHOWN ||
                 e.type == SDL_EVENT_WINDOW_EXPOSED) {
        // The window is visible now, so SDL_RaiseWindow's Cocoa activate/makeKey
        // actually takes (unlike the pre-loop attempt). One shot -- no retry.
        // Triggered by SHOWN or the first EXPOSED (first paint), whichever the
        // platform delivers, so a backend that omits SHOWN still activates.
        if (!raised_on_show) {
          SDL_RaiseWindow(window_);
          raised_on_show = true;
        }
      } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (width > 0 && height > 0) {
          gpu_.Configure(static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height));
          renderer_.Resize(static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height));
          view_->OnResize(width, height);
          ImGui_ImplWGPU_SetFramebufferSize(static_cast<uint32_t>(width),
                                            static_cast<uint32_t>(height));
        }
      } else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
        if (!ImGui::GetIO().WantCaptureKeyboard) {
          if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F2) {
            if (recorder_.active()) {
              recorder_.Stop();
            } else {
              recorder_.Start(kDefaultRecordDir);
            }
          }
          view_->HandleEvent(e, width, height);
        }
      } else {
        view_->HandleEvent(e, width, height);
      }
    }

    const uint64_t current_time = SDL_GetPerformanceCounter();
    const double frame_dt = perf_freq > 0
                                ? static_cast<double>(current_time - last_time) /
                                      static_cast<double>(perf_freq)
                                : (1.0 / 60.0);
    last_time = current_time;

    // The view advances its own SimClock (sim speed, day/night, fixed game
    // ticks) from this dt. Real wall time in the live window; a fixed step
    // while recording so captures are deterministic (bypasses the wall clock).
    const float pres_dt =
        recorder_.active() ? kPresentationDt : static_cast<float>(frame_dt);
    view_->Update(pres_dt, SDL_GetKeyboardState(nullptr));

    wgpu::TextureView surface = gpu_.AcquireSurfaceTexture();
    if (!surface) continue;

    // ImGui frame: started here (after the surface-acquire check) rather
    // than immediately after Update() so a skipped frame (surface
    // unavailable) never leaves an ImGui::NewFrame() unmatched by
    // ImGui::Render() -- ImGui asserts on back-to-back NewFrame() calls.
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // Mouse pos/buttons were already queued during the event loop above (via
    // mouse_input, in temporal order), so ImGui's trickle handling works exactly
    // as designed -- no post-NewFrame re-injection needed.
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    view_->DrawUI();

    // === UI overlay pass — BEFORE the scene render. Game UI + ImGui draw
    // into the RGBA8 overlay texture in plain sRGB with gamma-space blending
    // (the space the UI is authored for); the resolve pass then composites
    // the overlay over the scene in encoded space and owns all P3/EDR output
    // conversion (see SceneContext::ui_overlay). Drawing UI directly onto a
    // float/linear surface would hardware-blend in LINEAR space, visibly
    // brightening translucent panels and AA'd glyph edges.
    //
    // Two surfaces share this one pass, in order: the view's GAME UI first,
    // then Dear ImGui DEBUG UI on top. One encoder and one submit for both,
    // and the ordering is structural rather than a convention someone can
    // break.
    //
    // The game UI cannot borrow the renderer's frame uniform buffer here:
    // it draws outside SceneRenderer::Render's FrameContext. UiRenderer owns
    // its own small uniform buffer instead -- see engine/ui/ui_renderer.hpp.
    {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window_, &w, &h);
      EnsureUiOverlay(static_cast<uint32_t>(w), static_cast<uint32_t>(h));

      // Point the game UI at the overlay before any pass is open -- Prepare
      // may create a pipeline and writes the uniform buffer.
      if (UiRenderer* ui = view_->GetUiRenderer()) {
        ui->Prepare(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                    kUiOverlayFormat);
      }

      wgpu::CommandEncoder encoder = gpu_.GetDevice().CreateCommandEncoder();

      wgpu::RenderPassColorAttachment color_attachment;
      color_attachment.view = ui_overlay_view_;
      color_attachment.loadOp = wgpu::LoadOp::Clear;
      color_attachment.storeOp = wgpu::StoreOp::Store;
      color_attachment.clearValue = {0.0, 0.0, 0.0, 0.0};  // transparent
      color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

      wgpu::RenderPassDescriptor desc;
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color_attachment;
      desc.depthStencilAttachment = nullptr;

      wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);

      if (UiRenderer* ui = view_->GetUiRenderer()) ui->Draw(pass);

      ImGui::Render();
      ImDrawData* dd = ImGui::GetDrawData();
      // ImGui 1.92 obsoleted ImDrawData::CmdListsCount (now always 0); the live
      // list count is CmdLists.Size. Gating on the old field silently dropped
      // the entire debug overlay.
      if (dd && dd->CmdLists.Size > 0) ImGui_ImplWGPU_RenderDrawData(dd, pass);

      pass.End();
      wgpu::CommandBuffer commands = encoder.Finish();
      gpu_.GetQueue().Submit(1, &commands);
    }

    // Scene render + resolve (which composites the overlay). The overlay view
    // rides on a per-frame copy of the view's SceneContext -- the overlay is
    // app-owned presentation state, not scene state.
    {
      SceneContext scene = view_->GetSceneContext();
      scene.ui_overlay = ui_overlay_view_;
      renderer_.Render(view_->GetCamera(), view_->GetRegistry(), scene,
                       surface);
    }

    gpu_.Present();

    if (recorder_.active()) {
      // Deterministic capture size (config_, not HiDPI window pixels). Match
      // the camera aspect to the capture size for this offscreen render, then
      // restore the live window aspect so the on-screen view is unaffected.
      // renderer_.GetDebugMode() forwards the live debug visualization.
      view_->OnResize(config_.width, config_.height);
      recorder_.CaptureFrame(gpu_, *pipeline_gen_, *view_,
                             static_cast<uint32_t>(config_.width),
                             static_cast<uint32_t>(config_.height),
                             renderer_.GetDebugMode(),
                             renderer_.GetColorGradingConfig());
      view_->OnResize(width, height);
    }

#ifdef BADLANDS_PROFILING
    // Dump the accumulated scope profile to stderr roughly every 2 s (only when
    // BADLANDS_PROFILE is set). Called here at end-of-frame, after every
    // PROFILE_SCOPE this iteration has closed, so the reported tree is
    // well-formed. Report() also resets the buffers, so each dump is a ~2 s
    // window.
    if (kProfileDump) {
      profile_report_accum += frame_dt;
      if (profile_report_accum >= 2.0) {
        profiler::ReportToStderr();
        profile_report_accum = 0.0;
      }
    }
#endif

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
