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
#include "engine/app/screenshot.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/ui/imgui_impl_wgpu_custom.hpp"

namespace badlands {

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
  info.RenderTargetFormat = gpu_.GetSurfaceFormat();
  info.DepthStencilFormat = wgpu::TextureFormat::Undefined;  // 2D overlay, no depth
  info.FramebufferWidth = static_cast<uint32_t>(width);
  info.FramebufferHeight = static_cast<uint32_t>(height);
  info.OutputIsLinear =
      (gpu_.GetSurfaceFormat() == wgpu::TextureFormat::RGBA16Float);  // false for BGRA8
  ImGui_ImplWGPU_Init(&info);

  spdlog::info("SdlViewerApp: ImGui initialized");
}

namespace {
// Default recording directory for the F2 live-toggle (no path argument).
constexpr const char* kDefaultRecordDir = "recordings";

// --- Input/focus instrumentation ---------------------------------------------
// Opt-in (BADLANDS_INPUT_DEBUG=1) diagnosis for the "window loses input" bug:
// macOS routes mouse MOTION to the window under the cursor even when it is not
// the key window, but withholds keyboard + swallows the activating click -- so
// symptoms are "hover works, WSAD/clicks dead". This logs every window/input
// EVENT as it happens (they are sparse), and a full state SNAPSHOT at a fixed
// interval (never per frame). The load-bearing field is SDL_WINDOW_INPUT_FOCUS:
// when broken it should read 0 while SDL_WINDOW_MOUSE_FOCUS reads 1.
const bool kInputDebug = [] {
  const char* v = std::getenv("BADLANDS_INPUT_DEBUG");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}();

const char* WindowEventName(uint32_t t) {
  switch (t) {
    case SDL_EVENT_WINDOW_SHOWN: return "SHOWN";
    case SDL_EVENT_WINDOW_HIDDEN: return "HIDDEN";
    case SDL_EVENT_WINDOW_EXPOSED: return "EXPOSED";
    case SDL_EVENT_WINDOW_MOVED: return "MOVED";
    case SDL_EVENT_WINDOW_RESIZED: return "RESIZED";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "PIXEL_SIZE_CHANGED";
    case SDL_EVENT_WINDOW_MINIMIZED: return "MINIMIZED";
    case SDL_EVENT_WINDOW_MAXIMIZED: return "MAXIMIZED";
    case SDL_EVENT_WINDOW_RESTORED: return "RESTORED";
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return "MOUSE_ENTER";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "MOUSE_LEAVE";
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return "FOCUS_GAINED";
    case SDL_EVENT_WINDOW_FOCUS_LOST: return "FOCUS_LOST";
    case SDL_EVENT_WINDOW_OCCLUDED: return "OCCLUDED";
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: return "ENTER_FULLSCREEN";
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: return "LEAVE_FULLSCREEN";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return "DISPLAY_CHANGED";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return "CLOSE_REQUESTED";
    default: return "WINDOW_?";
  }
}

// Per-interval event tallies (motion/wheel are too frequent to log each).
struct InputCounts {
  int motions = 0, wheels = 0, buttons = 0, keys = 0;
};

// Log one event if it is worth a line (window state changes + discrete button /
// key presses). Motion + wheel are only counted (into `c`), never logged.
void LogInputEvent(const SDL_Event& e, InputCounts& c) {
  if (!kInputDebug) return;
  if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) {
    spdlog::info("[input] event WINDOW_{}", WindowEventName(e.type));
    return;
  }
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: ++c.motions; break;
    case SDL_EVENT_MOUSE_WHEEL: ++c.wheels; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      ++c.buttons;
      spdlog::info("[input] event MOUSE_BUTTON_{} button={} at ({:.0f},{:.0f})",
                   e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "DOWN" : "UP",
                   e.button.button, e.button.x, e.button.y);
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      ++c.keys;
      spdlog::info("[input] event KEY_{} key='{}' scancode={} repeat={}",
                   e.type == SDL_EVENT_KEY_DOWN ? "DOWN" : "UP",
                   SDL_GetKeyName(e.key.key), static_cast<int>(e.key.scancode),
                   e.key.repeat);
      break;
    case SDL_EVENT_TEXT_INPUT:
      spdlog::info("[input] event TEXT_INPUT '{}'", e.text.text);
      break;
    default: break;
  }
}

// Full input/focus state, thrown out at a fixed interval (not per frame).
void LogInputSnapshot(SDL_Window* w, InputCounts& c) {
  if (!kInputDebug || ImGui::GetCurrentContext() == nullptr) return;
  const SDL_WindowFlags f = SDL_GetWindowFlags(w);
  const ImGuiIO& io = ImGui::GetIO();
  float gx = 0, gy = 0, wx = 0, wy = 0;
  SDL_GetGlobalMouseState(&gx, &gy);
  SDL_GetMouseState(&wx, &wy);
  const bool* ks = SDL_GetKeyboardState(nullptr);
  spdlog::info(
      "[input] SNAPSHOT win[inputFocus={} mouseFocus={} minimized={} hidden={} "
      "occluded={} kbdGrab={} mouseGrab={}] imgui[wantMouse={} wantKbd={} "
      "wantText={} navActive={} mouseDown={}{}{} mousePos=({:.0f},{:.0f})] "
      "sdl[global=({:.0f},{:.0f}) win=({:.0f},{:.0f}) WSAD={}{}{}{}] "
      "since[motion={} wheel={} button={} key={}]",
      (f & SDL_WINDOW_INPUT_FOCUS) != 0, (f & SDL_WINDOW_MOUSE_FOCUS) != 0,
      (f & SDL_WINDOW_MINIMIZED) != 0, (f & SDL_WINDOW_HIDDEN) != 0,
      (f & SDL_WINDOW_OCCLUDED) != 0, (f & SDL_WINDOW_KEYBOARD_GRABBED) != 0,
      (f & SDL_WINDOW_MOUSE_GRABBED) != 0, io.WantCaptureMouse,
      io.WantCaptureKeyboard, io.WantTextInput, io.NavActive,
      static_cast<int>(io.MouseDown[0]), static_cast<int>(io.MouseDown[1]),
      static_cast<int>(io.MouseDown[2]), io.MousePos.x, io.MousePos.y, gx, gy, wx,
      wy, static_cast<int>(ks[SDL_SCANCODE_W]), static_cast<int>(ks[SDL_SCANCODE_A]),
      static_cast<int>(ks[SDL_SCANCODE_S]), static_cast<int>(ks[SDL_SCANCODE_D]),
      c.motions, c.wheels, c.buttons, c.keys);
  c = InputCounts{};  // reset tallies for the next interval
}
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
#ifdef BADLANDS_PROFILING
  // Per-pass GPU timing in the live window (prints alongside the CPU profile).
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
                             renderer_.GetShadowDebugMode(), screenshot_time);
    return ok ? 0 : 1;
  }

  if (!record_dir.empty()) {
    recorder_.Start(record_dir);
  }

  // Make the window the key/active window on launch. A bare (non-bundled) macOS
  // executable launched from a shell frequently opens WITHOUT input focus, so it
  // shows but keyboard/clicks go to whatever was focused before -- until you
  // click it. Raise it to request focus so WSAD/clicks work from the first frame
  // (watch BADLANDS_INPUT_DEBUG's inputFocus field).
  SDL_RaiseWindow(window_);

  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  uint64_t last_time = SDL_GetPerformanceCounter();
#ifdef BADLANDS_PROFILING
  double profile_report_accum = 0.0;  // dump the scope profile every ~2 s
#endif

  bool render_ok_logged = false;
  bool running = true;
  InputCounts input_counts;
  double snapshot_accum = 0.0;
  if (kInputDebug) {
    const SDL_WindowFlags f0 = SDL_GetWindowFlags(window_);
    spdlog::info("[input] START win[inputFocus={} mouseFocus={} minimized={}] "
                 "(reproduce the stuck state, then watch inputFocus vs the "
                 "FOCUS_GAINED/RESTORED events)",
                 (f0 & SDL_WINDOW_INPUT_FOCUS) != 0,
                 (f0 & SDL_WINDOW_MOUSE_FOCUS) != 0,
                 (f0 & SDL_WINDOW_MINIMIZED) != 0);
  }
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      LogInputEvent(e, input_counts);

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
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    // Throttled input/focus snapshot (after NewFrame so io.WantCapture* are for
    // this frame). Never per frame -- once a second, and only when opted in.
    snapshot_accum += frame_dt;
    if (snapshot_accum >= 1.0) {
      snapshot_accum = 0.0;
      LogInputSnapshot(window_, input_counts);
    }

    view_->DrawUI();

    renderer_.Render(view_->GetCamera(), view_->GetRegistry(),
                     view_->GetSceneContext(), surface);

    // App-owned ImGui composite pass: a SEPARATE render pass on the same
    // surface view, loading (not clearing) the scene the renderer just
    // wrote, with no depth attachment (ImGui is a 2D overlay).
    {
      wgpu::CommandEncoder encoder = gpu_.GetDevice().CreateCommandEncoder();

      wgpu::RenderPassColorAttachment color_attachment;
      color_attachment.view = surface;
      color_attachment.loadOp = wgpu::LoadOp::Load;
      color_attachment.storeOp = wgpu::StoreOp::Store;
      color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

      wgpu::RenderPassDescriptor desc;
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color_attachment;
      desc.depthStencilAttachment = nullptr;

      wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);

      ImGui::Render();
      ImDrawData* dd = ImGui::GetDrawData();
      if (dd && dd->CmdListsCount > 0) ImGui_ImplWGPU_RenderDrawData(dd, pass);

      pass.End();
      wgpu::CommandBuffer commands = encoder.Finish();
      gpu_.GetQueue().Submit(1, &commands);
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
                             renderer_.GetDebugMode());
      view_->OnResize(width, height);
    }

#ifdef BADLANDS_PROFILING
    // Dump the accumulated scope profile to stderr roughly every 2 s. Called
    // here at end-of-frame, after every PROFILE_SCOPE this iteration has
    // closed, so the reported tree is well-formed. Report() also resets the
    // buffers, so each dump is a ~2 s window.
    profile_report_accum += frame_dt;
    if (profile_report_accum >= 2.0) {
      profiler::ReportToStderr();
      profile_report_accum = 0.0;
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
