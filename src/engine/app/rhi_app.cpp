#include "engine/app/rhi_app.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

#include "badlands_assets.h"
#include "engine/app/fixed_timestep.hpp"
#include "engine/app/platform_surface.hpp"
#include "engine/graph/render_graph.hpp"
#include "engine/app/ui_compositor.hpp"
#include "engine/ui/imgui_impl_rhi.hpp"

namespace badlands::rhi_app {

using namespace badlands::rhi;

namespace {

// The overlay ImGui draws into. ENCODED and 8-bit, which is the space UI is
// authored for -- see ui_composite.slang for why this is not optional.
constexpr Format kUiFormat = Format::RGBA8Unorm;

constexpr uint32_t kCompositeParamsSlot = 0;
constexpr uint32_t kCompositeUiSlot = 1;

struct CompositeParams {
  float mode[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

bool ParseFloatArg(const char* text, float& out) {
  char* end = nullptr;
  const double v = std::strtod(text, &end);
  if (end == text || *end != '\0' || v <= 0.0) return false;
  out = float(v);
  return true;
}

bool ParseU64Arg(const char* text, uint64_t& out) {
  // A LEADING MINUS IS REFUSED EXPLICITLY. strtoull accepts one and NEGATES it,
  // so "-1" parses successfully as 18446744073709551615 -- a frame cap that
  // silently means "forever" instead of being rejected. object_viewer's own
  // parser refused it before this layer took the flag over, and its ctest entry
  // caught the regression immediately.
  if (!text || *text == '-') return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE) return false;
  out = uint64_t(v);
  return true;
}

}  // namespace

RhiAppOptions ParseAppArgs(int& argc, char** argv) {
  RhiAppOptions opt;
  // REMOVED as they are consumed, not merely read. A flag the layer advertises
  // but leaves in argv reaches the app's own parser as an unknown argument and
  // gets refused there -- which reads as the app rejecting a flag the layer
  // documents.
  int out = 1;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto value = [&](const char*& v) {
      if (i + 1 >= argc) {
        spdlog::error("rhi_app: {} needs a value", a);
        opt.valid = false;
        return false;
      }
      v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (std::strcmp(a, "--frames") == 0) {
      if (!value(v)) break;
      if (!ParseU64Arg(v, opt.max_frames)) {
        spdlog::error("rhi_app: --frames wants a whole number, got '{}'", v);
        opt.valid = false;
      }
    } else if (std::strcmp(a, "--fixed-dt") == 0) {
      if (!value(v)) break;
      if (!ParseFloatArg(v, opt.fixed_dt)) {
        spdlog::error("rhi_app: --fixed-dt wants a positive number, got '{}'", v);
        opt.valid = false;
      }
    } else if (std::strcmp(a, "--screenshot") == 0) {
      if (!value(v)) break;
      opt.screenshot_path = v;
    } else {
      argv[out++] = argv[i];
    }
  }
  argc = out;
  return opt;
}

namespace {

// The host, implemented over the shell. A thin forwarder rather than exposing
// AppShell itself, so a view cannot re-enter Run or reach the swapchain.
class ShellHost final : public RhiAppHost {
 public:
  explicit ShellHost(AppShell& shell) : shell_(shell) {}
  void RequestResizePoints(uint32_t w, uint32_t h) override {
    shell_.RequestResizePoints(w, h);
  }
  void Stop() override { shell_.Stop(); }
  uint32_t SwapchainWidth() const override {
    return shell_.Swapchain() ? shell_.Swapchain()->GetWidth() : 0;
  }
  uint32_t SwapchainHeight() const override {
    return shell_.Swapchain() ? shell_.Swapchain()->GetHeight() : 0;
  }
  SDL_Window* Window() override { return shell_.Window(); }

 private:
  AppShell& shell_;
};

}  // namespace

int RhiApp::Run(int argc, char** argv, const ViewFactory& factory,
                RunStats* out_stats) {
  RhiAppOptions opt = ParseAppArgs(argc, argv);
  if (!opt.valid) return 1;
  return RunParsed(opt, factory, out_stats);
}

int RhiApp::RunParsed(const RhiAppOptions& opt, const ViewFactory& factory,
                      RunStats* out_stats) {
  if (!opt.valid) return 1;

  // Borrowed if the app already has one -- see RhiAppConfig::device. `owned`
  // exists only to keep the layer-created case alive for the run.
  std::unique_ptr<IRhiDevice> owned;
  IRhiDevice* device = config_.device;
  if (!device) {
    owned = CreateDevice({.backend = NativeBackend(),
                          .enable_validation = true,
                          .label = config_.title});
    device = owned.get();
  }
  if (!device) return 1;

  auto compiler = slang::CreateSlangCompiler(config_.shader_paths);
  if (!compiler) return 1;

  auto shell = AppShell::Create(*device, {.title = config_.title,
                                          .width = config_.width,
                                          .height = config_.height,
                                          .present_format = Format::BGRA8Unorm,
                                          .color_space = config_.color_space,
                                          .prefer_hdr = config_.prefer_hdr,
                                          .vsync = config_.vsync});
  if (!shell) return 1;

  // READ BACK, never assumed: the shell may have upgraded to RGBA16Float on an
  // HDR display, and the swapchain may have dropped off it again if the layer
  // refused to tag.
  const Format surface_format = shell->SurfaceFormat();
  const ColorSpace surface_cs = shell->SurfaceColorSpace();
  spdlog::info("rhi_app: presenting {} / {}", ToString(surface_format),
               ToString(surface_cs));

  // --- ImGui, entirely the layer's ---------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  if (!ImGui_ImplSDL3_InitForMetal(shell->Window())) {
    spdlog::error("rhi_app: ImGui_ImplSDL3_InitForMetal failed");
    return 1;
  }
  if (!ImGui_ImplRHI_Init({.device = device,
                           .compiler = compiler.get(),
                           .target_format = kUiFormat,
                           .framebuffer_width = shell->Width(),
                           .framebuffer_height = shell->Height()})) {
    return 1;
  }

  auto compositor = UiCompositor::Create(*device, *compiler, surface_format,
                                        surface_cs, shell->Width(),
                                        shell->Height());
  if (!compositor) return 1;

  // --- The view -----------------------------------------------------------
  ShellHost host(*shell);
  RhiAppContext ctx{.host = &host,
                    .device = device,
                    .compiler = compiler.get(),
                    .surface_format = surface_format,
                    .surface_color_space = surface_cs,
                    .width = shell->Width(),
                    .height = shell->Height()};
  auto view = factory(ctx);
  if (!view || !view->Initialize(ctx)) {
    spdlog::error("rhi_app: the view could not be initialised");
    return 1;
  }

  // --- Time ---------------------------------------------------------------
  FrameClock clock{.fixed_dt = opt.fixed_dt};

  // --- The loop -----------------------------------------------------------
  AppShellCallbacks cb;
  cb.OnEvent = [&](const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    // ImGui gets FIRST REFUSAL on input it is using, so a drag inside a panel
    // does not also move the camera. Escape and quit never reach here -- the
    // shell acts on them first, because a focused widget otherwise swallows
    // every key-down and "Esc to quit" silently stops working.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse &&
        (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
      return true;
    }
    if (io.WantCaptureKeyboard &&
        (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)) {
      return true;
    }
    return view->OnEvent(e);
  };
  cb.OnResize = [&](uint32_t w, uint32_t h) {
    if (!compositor->Resize(w, h)) {
      spdlog::error("rhi_app: could not rebuild the UI overlay at {}x{}", w, h);
      return false;
    }
    ImGui_ImplRHI_SetFramebufferSize(w, h);
    return view->OnResize(w, h);
  };
  cb.OnUpdate = [&](const FrameInfo& f) {
    // WALL CLOCK, or exactly the fixed step -- see FrameClock, which is where
    // this logic lives so it can be tested without a window.
    view->Update(clock.Advance(f.dt, f.index, f.keys));
  };
  cb.OnFrameBegin = [&](uint64_t frame_index) {
    view->OnFrameBegin(frame_index);
    ImGui_ImplRHI_NewFrame(frame_index);
  };

  TextureReadbackPtr pending_shot;
  cb.OnRender = [&](ITextureView* target, const FrameInfo& f) {
    ImGui_ImplRHI_SetFramebufferSize(f.width, f.height);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    view->DrawUI();
    ImGui::Render();

    if (!view->Render(target, f)) return false;

    // THE UI GOES LAST, so debug UI always sits on top of whatever the app
    // drew. Its own graph, submitted after the view's: the view owns its
    // passes and this layer does not reach into them.
    graph::RenderGraph graph(*device);
    auto surface_h = graph.ImportTexture(target->GetTexture(),
                                         ResourceState::Undefined, "surface");
    auto ui_h = compositor->BeginOverlay(graph);
    if (!ui_h.IsValid() || !surface_h.IsValid()) return false;
    ImGui_ImplRHI_AddPass(ImGui::GetDrawData(), graph, ui_h);
    // THE UI GOES LAST, so debug UI always sits on top of whatever the app drew.
    if (!compositor->Composite(graph, ui_h, surface_h)) return false;

    // COMPILED before executed. Execute refuses to record passes whose
    // declarations were never checked, and it says so -- which is how this got
    // caught rather than silently dropping the UI on the first run.
    if (!graph.Compile()) return false;
    auto encoder = device->CreateCommandEncoder("rhi_app_ui");
    graph.Execute(*encoder);

    // The screenshot reads the BACKBUFFER, after the view and after the UI --
    // so it captures what was presented, and the layer needs to know nothing
    // about the app's passes. Recorded into this same encoder, before it is
    // finished, because Metal wants the completion handler attached pre-commit.
    if (!opt.screenshot_path.empty() && !pending_shot) {
      pending_shot = device->ReadTexture(*encoder, target);
    }
    encoder->Finish();
    device->Submit(*encoder);
    return true;
  };

  const uint64_t frame_cap =
      !opt.screenshot_path.empty() && opt.max_frames == 0 ? 1 : opt.max_frames;
  const RunStats stats = shell->Run(cb, frame_cap);
  if (out_stats) *out_stats = stats;

  int exit_code = stats.aborted ? 1 : 0;
  if (pending_shot) {
    // WAIT, not OnComplete: the process exits immediately after this, and an
    // event delivered once the loop has stopped would be delivered never.
    if (!pending_shot->Wait()) {
      exit_code = 1;
    } else {
      const auto data = pending_shot->Data();
      std::vector<uint8_t> rgba(data.begin(), data.end());
      // BGRA on the wire (CAMetalLayer takes nothing else), RGBA in a PNG.
      if (pending_shot->GetFormat() == Format::BGRA8Unorm ||
          pending_shot->GetFormat() == Format::BGRA8UnormSrgb) {
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
          std::swap(rgba[i], rgba[i + 2]);
        }
      }
      badlands_write_png(opt.screenshot_path.c_str(), rgba.data(),
                         pending_shot->GetWidth(), pending_shot->GetHeight());
      spdlog::info("rhi_app: wrote {} ({}x{})", opt.screenshot_path,
                   pending_shot->GetWidth(), pending_shot->GetHeight());
    }
    pending_shot.reset();
  }

  // Shutdown in reverse: the view's resources before the device that made
  // them, and ImGui's backend before its context.
  view.reset();
  compositor.reset();
  ImGui_ImplRHI_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  spdlog::info("rhi_app: {} frames, {} presented", stats.frames_begun,
               stats.frames_presented);
  return exit_code;
}

}  // namespace badlands::rhi_app
