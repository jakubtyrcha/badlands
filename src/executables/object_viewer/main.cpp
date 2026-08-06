// object_viewer -- the next iteration of badlands_viewer, on the native RHI and
// the render graph instead of Dawn.
//
// Stage 1: an empty screen. That is not a warm-up -- it is the smallest program
// that pins the graph's hardest interface, because windowed and headless differ
// ONLY in which texture gets imported as the graph's output:
//
//   windowed   swapchain->Acquire() -> the drawable's texture
//   headless   a plain texture, read back and written to a PNG
//
// Everything else -- the passes, the graph, the recording -- is the same code
// down to the line. A headless run is therefore not a separate path that can
// rot; it is the real one, with a different sink.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "badlands_assets.h"
#include "engine/app/rhi_app_shell.hpp"
#include "engine/graph/render_graph.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "engine/ui/imgui_impl_rhi.hpp"
#include "executables/object_viewer/line_pass.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

using namespace badlands;
using namespace badlands::rhi;
using badlands::graph::RasterContext;
using badlands::graph::RenderGraph;
using badlands::object_viewer::LinePass;

namespace {

// What the frame contains. A selector rather than a flag because each scene
// carries its OWN pixel assertion -- "every texel is the clear colour" and "a
// segment covers these texels and not those" are different claims, and a
// headless run that could not say which it was checking would be checking
// neither.
enum class Scene { Clear, Lines, Grid };

struct Options {
  bool headless = false;
  // Pushes a synthetic Escape mid-run while OnEvent consumes EVERY event, and
  // fails unless the loop stopped anyway. The only way to test that Escape
  // survives an ImGui panel holding keyboard focus, which is what broke it.
  bool self_test_escape = false;
  Scene scene = Scene::Clear;
  uint32_t width = 1280;
  uint32_t height = 720;
  std::string out = "object_viewer.png";
  uint64_t max_frames = 0;  // 0 = until quit
  // The clear colour, so a headless run has something falsifiable to assert.
  float clear[4] = {0.05f, 0.06f, 0.09f, 1.0f};
};

// Parses an unsigned argument, refusing anything that would truncate or wrap.
// The lab learned this the hard way: `--width 4294967297` silently became 1 and
// rendered a valid-looking image at the wrong size with exit status 0.
bool ParseU32(const char* text, uint32_t min, uint32_t max, uint32_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || v < min || v > max) return false;
  out = uint32_t(v);
  return true;
}

bool ParseArgs(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char*& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (a == "--headless") {
      opt.headless = true;
    } else if (a == "--self-test-escape") {
      opt.self_test_escape = true;
    } else if (a == "--scene") {
      if (!value(v)) return false;
      if (std::strcmp(v, "clear") == 0) opt.scene = Scene::Clear;
      else if (std::strcmp(v, "lines") == 0) opt.scene = Scene::Lines;
      else if (std::strcmp(v, "grid") == 0) opt.scene = Scene::Grid;
      else {
        spdlog::error("object_viewer: unknown scene '{}' (clear|lines|grid)", v);
        return false;
      }
    } else if (a == "--width") {
      if (!value(v) || !ParseU32(v, 1, 16384, opt.width)) return false;
    } else if (a == "--height") {
      if (!value(v) || !ParseU32(v, 1, 16384, opt.height)) return false;
    } else if (a == "--frames") {
      uint32_t n = 0;
      if (!value(v) || !ParseU32(v, 1, 100000, n)) return false;
      opt.max_frames = n;
    } else if (a == "--out") {
      if (!value(v)) return false;
      opt.out = v;
      opt.headless = true;  // asking for a file implies headless
    } else {
      spdlog::error("object_viewer: unknown argument '{}'", a);
      return false;
    }
  }
  return true;
}

// A fixed camera for the headless scenes, and the starting camera for the
// windowed one. Orthographic so a world segment maps to a screen span in closed
// form, which is what lets the line assertion name exact pixels rather than
// eyeball a picture.
struct Camera {
  glm::vec3 position{0.0f, 0.0f, 10.0f};
  float extent = 10.0f;  // half-height of the ortho box, in world units
  // Radians. Both zero looks straight down -Z, which is what the line scene
  // needs: its assertion names exact pixels, and that closed form only holds
  // for an unrotated camera. The grid scene pitches down, because a ground
  // plane viewed edge-on is a single line -- which is exactly what the first
  // attempt rendered.
  float pitch = 0.0f;
  float yaw = 0.0f;

  glm::vec3 Forward() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            -std::cos(pitch) * std::cos(yaw)};
  }

  // The camera-OFFSET matrices ExpandDebugLines documents: the camera sits at
  // the origin and the world is rebased by -position.
  glm::mat4 View() const {
    return glm::lookAt(glm::vec3(0.0f), Forward(), glm::vec3(0, 1, 0));
  }
  glm::mat4 Proj(float aspect) const {
    // near and far SWAPPED, which is how reversed-Z is spelled with
    // GLM_FORCE_DEPTH_ZERO_TO_ONE: the near plane maps to 1 and the far to 0.
    return glm::ortho(-extent * aspect, extent * aspect, -extent, extent,
                      100.0f, 0.0f);
  }
};

// The camera each scene is asserted under. The line scene MUST stay unrotated:
// its assertion is a closed form ("half the width, middle row"), and that only
// holds looking straight down -Z.
Camera CameraFor(Scene scene) {
  Camera cam;
  if (scene == Scene::Grid) {
    cam.position = {0.0f, 6.0f, 14.0f};
    cam.pitch = -0.45f;
    cam.yaw = 0.35f;
    cam.extent = 12.0f;
  }
  return cam;
}

// One horizontal segment through the world origin. Deliberately the simplest
// thing that can be asserted on: with an orthographic camera it covers the
// middle row of the image and nothing else, so "did the line pass run" and "did
// it run in the right place" are the same question.
DebugLineBuffer LineScene() {
  DebugLineBuffer lines;
  lines.AddLine({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f},
                {1.0f, 0.0f, 0.0f}, /*thickness=*/4.0f);
  return lines;
}

// What a viewer actually wants on screen: a ground grid and the three axes.
DebugLineBuffer GridScene() {
  DebugLineBuffer lines;
  constexpr int kHalf = 10;
  constexpr float kStep = 1.0f;
  const glm::vec3 grey{0.30f, 0.32f, 0.38f};
  for (int i = -kHalf; i <= kHalf; ++i) {
    const float t = float(i) * kStep;
    const float e = float(kHalf) * kStep;
    lines.AddLine({t, 0, -e}, {t, 0, e}, grey, 1.0f);
    lines.AddLine({-e, 0, t}, {e, 0, t}, grey, 1.0f);
  }
  lines.AddLine({0, 0, 0}, {3, 0, 0}, {0.9f, 0.2f, 0.2f}, 3.0f);
  lines.AddLine({0, 0, 0}, {0, 3, 0}, {0.2f, 0.9f, 0.2f}, 3.0f);
  lines.AddLine({0, 0, 0}, {0, 0, 3}, {0.3f, 0.4f, 0.95f}, 3.0f);
  return lines;
}

// THE GRAPH, built identically for both modes. `sink` is whatever the caller
// wants rendered into; the graph neither knows nor cares whether a display is
// attached.
bool BuildGraph(RenderGraph& graph, ITexture* sink, const Options& opt,
                LinePass* lines) {
  // Undefined on entry every frame: a freshly acquired drawable is a NEW
  // resource each time, so any state carried over from last frame would be a
  // lie. Stating it here rather than assuming it is what the entry-state
  // parameter is for.
  auto out = graph.ImportTexture(sink, ResourceState::Undefined, "sink");
  if (!out.IsValid()) return false;

  graph.AddRasterPass("clear")
      .ColorTarget(out, LoadOp::Clear, StoreOp::Store, opt.clear)
      .Execute([](const RasterContext&) {
        // The clear IS this pass. It exists as its own pass so the target has a
        // defined starting state whether or not anything draws afterwards.
      });

  // A SECOND pass into the same target, loading rather than clearing. Two
  // passes over one attachment is the smallest case that makes the graph's
  // ordering and its RenderTarget state carry weight.
  if (lines) lines->AddToGraph(graph, out, LoadOp::Load);
  return graph.Compile();
}

// The Slang compiler, created only when a scene needs a shader. The clear scene
// does not, so a run without a Slang SDK still proves the graph.
std::unique_ptr<slang::SlangCompiler> MakeCompiler() {
  // Both roots: the line shader lives with the viewer, the ImGui shader with
  // the other UI shaders, and each resolves its own imports.
  const std::vector<std::string> paths = {"shaders/slang/object_viewer",
                                          "shaders/slang/ui"};
  return slang::CreateSlangCompiler(paths);
}

int RunHeadless(IRhiDevice& device, const Options& opt) {
  auto sink = device.CreateTexture({.width = opt.width,
                                    .height = opt.height,
                                    .format = Format::RGBA8Unorm,
                                    .usage = TextureUsage::RenderTarget |
                                             TextureUsage::CopySrc,
                                    .label = "sink"});
  auto readback = device.CreateBuffer(
      {.size = uint64_t(opt.width) * opt.height * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "readback"});
  if (!sink || !readback) return 1;

  std::unique_ptr<slang::SlangCompiler> compiler;
  std::unique_ptr<LinePass> lines;
  if (opt.scene != Scene::Clear) {
    compiler = MakeCompiler();
    if (!compiler) return 1;
    lines = LinePass::Create(device, *compiler, Format::RGBA8Unorm);
    if (!lines) return 1;
  }

  device.BeginValidationScope();
  device.BeginFrame();
  if (lines) {
    lines->BeginFrame(device.CurrentFrame());
    const Camera cam = CameraFor(opt.scene);
    const float aspect = float(opt.width) / float(opt.height);
    const DebugLineBuffer scene =
        opt.scene == Scene::Grid ? GridScene() : LineScene();
    if (!lines->Upload(scene, cam.View(), cam.Proj(aspect),
                       {float(opt.width), float(opt.height)}, cam.position)) {
      return 1;
    }
  }

  RenderGraph graph(device);
  if (!BuildGraph(graph, sink.get(), opt, lines.get())) return 1;

  auto encoder = device.CreateCommandEncoder("frame");
  graph.Execute(*encoder);
  // The readback is the CALLER's, not the graph's: copying out is a property of
  // this run, not of the passes, and putting it in the graph would give the
  // windowed path a copy it never performs.
  encoder->Transition(sink.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(sink.get(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  device.EndFrame();
  device.WaitIdle();

  if (auto report = device.EndValidationScope();
      report && !report->IsClean()) {
    spdlog::error("object_viewer: validation observed: {}", report->violations);
    return 1;
  }

  std::vector<uint8_t> pixels(size_t(opt.width) * opt.height * 4, 0);
  if (!readback->Read(0, pixels)) {
    spdlog::error("object_viewer: readback failed");
    return 1;
  }
  // THE ASSERTION, and the reason the headless ctest means anything. Exit
  // status IS the check; there is no test framework around this, and writing a
  // PNG and exiting 0 would pass just as well against a graph that recorded no
  // pass at all.
  auto expect = [](float v) {
    return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t want[4] = {expect(opt.clear[0]), expect(opt.clear[1]),
                           expect(opt.clear[2]), expect(opt.clear[3])};
  auto at = [&](uint32_t x, uint32_t y) {
    return &pixels[(size_t(y) * opt.width + x) * 4];
  };
  auto is_clear = [&](const uint8_t* p) {
    for (int c = 0; c < 4; ++c) {
      if (std::abs(int(p[c]) - int(want[c])) > 1) return false;
    }
    return true;
  };

  if (opt.scene == Scene::Clear) {
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (!is_clear(&pixels[i])) {
        spdlog::error(
            "object_viewer: texel {} is rgba({},{},{},{}) but the graph was "
            "asked to clear to rgba({},{},{},{})",
            i / 4, pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3],
            want[0], want[1], want[2], want[3]);
        return 1;
      }
    }
    spdlog::info("object_viewer: every texel is rgba({},{},{},{})", want[0],
                 want[1], want[2], want[3]);
  } else if (opt.scene == Scene::Grid) {
    // The grid is what the viewer actually shows, so it is rendered rather than
    // trusted. Its assertion is deliberately weak -- it is a picture, not a
    // measurement -- but "a lot of texels changed and the origin is lit" still
    // separates a drawn grid from a blank frame.
    size_t lit = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (!is_clear(&pixels[i])) ++lit;
    }
    if (lit < pixels.size() / 4 / 100) {
      spdlog::error("object_viewer: only {} lit texels -- the grid is missing",
                    lit);
      return 1;
    }
    spdlog::info("object_viewer: grid drew {} lit texels", lit);
  } else {
    // The segment spans world x in [-5, 5] at y = 0, under an orthographic
    // camera of half-height 10. So it covers the middle ROW and the middle
    // HALF of the width, and nothing else -- three claims, each falsifiable.
    const uint32_t mid_y = opt.height / 2;
    const uint32_t centre_x = opt.width / 2;
    size_t lit = 0;
    for (uint32_t x = 0; x < opt.width; ++x) {
      if (!is_clear(at(x, mid_y))) ++lit;
    }
    const uint8_t* centre = at(centre_x, mid_y);
    if (is_clear(centre)) {
      spdlog::error(
          "object_viewer: the centre texel is still the clear colour -- the "
          "line pass drew nothing");
      return 1;
    }
    if (centre[0] < 128 || centre[1] > 64) {
      spdlog::error("object_viewer: the centre texel is rgba({},{},{},{}), not "
                    "the red the segment was given",
                    centre[0], centre[1], centre[2], centre[3]);
      return 1;
    }
    // Half the width, within the antialias fringe on either end.
    const size_t want_lit = opt.width / 2;
    if (lit < want_lit - 4 || lit > want_lit + 4) {
      spdlog::error(
          "object_viewer: {} lit texels across the middle row, expected about "
          "{} for a segment spanning half the view",
          lit, want_lit);
      return 1;
    }
    // THE FRINGE, which is the only place blending is observable. The shader
    // emits a 1px alpha ramp at the quad's edge; blended, that composites
    // against the clear colour and the target stays opaque. Without blending
    // the shader's own alpha is written straight through, so the fringe texel
    // comes back with alpha 128 instead of 255 -- and every other assertion in
    // this scene passes either way, which is exactly why this one is here.
    bool found_fringe = false;
    for (uint32_t y = 0; y < opt.height; ++y) {
      const uint8_t* p = at(centre_x, y);
      if (is_clear(p)) continue;
      const bool core = p[0] > 250 && p[1] < 8 && p[2] < 8 && p[3] == 255;
      if (core) continue;
      found_fringe = true;
      if (p[3] != 255) {
        spdlog::error(
            "object_viewer: fringe texel at y={} has alpha {} -- the shader's "
            "alpha reached the target unblended",
            y, p[3]);
        return 1;
      }
      if (p[0] <= want[0] || p[0] >= 255) {
        spdlog::error(
            "object_viewer: fringe texel at y={} is rgba({},{},{},{}), not a "
            "blend of the line colour and the clear colour",
            y, p[0], p[1], p[2], p[3]);
        return 1;
      }
    }
    if (!found_fringe) {
      spdlog::error(
          "object_viewer: no antialias fringe anywhere in the centre column -- "
          "the line has hard edges");
      return 1;
    }

    // ...and the corners are untouched, so the line is a line and not a fill.
    for (auto [x, y] : {std::pair<uint32_t, uint32_t>{0, 0},
                        {opt.width - 1, 0},
                        {0, opt.height - 1},
                        {opt.width - 1, opt.height - 1}}) {
      if (!is_clear(at(x, y))) {
        spdlog::error("object_viewer: corner ({},{}) is not the clear colour",
                      x, y);
        return 1;
      }
    }
    spdlog::info("object_viewer: {} lit texels across the middle row, corners "
                 "clear", lit);
  }

  badlands_write_png(opt.out.c_str(), pixels.data(), opt.width, opt.height);
  spdlog::info("object_viewer: wrote {} ({}x{})", opt.out, opt.width,
               opt.height);
  return 0;
}

int RunWindowed(IRhiDevice& device, const Options& opt) {
  auto shell = rhi_app::AppShell::Create(device, {.title = "badlands object_viewer",
                                                  .width = opt.width,
                                                  .height = opt.height,
                                                  .present_format =
                                                      Format::BGRA8Unorm});
  if (!shell) return 1;

  // BGRA, because that is what CAMetalLayer accepts and the pipeline's colour
  // format has to match its attachment. The shader is unchanged either way --
  // channel order is the hardware's business.
  //
  // The compiler and the line pass exist only if a scene needs them. --scene
  // clear paid for both before, which is the accepted-and-ignored trap from the
  // other direction: a flag that changed nothing still changed the cost.
  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  std::unique_ptr<LinePass> lines;
  if (opt.scene != Scene::Clear) {
    lines = LinePass::Create(device, *compiler, Format::BGRA8Unorm);
    if (!lines) return 1;
  }

  // The DEBUG UI surface, distinct from any in-world game UI. imgui_impl_sdl3
  // is the platform half (already vendored); imgui_impl_rhi is the renderer.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  if (!ImGui_ImplSDL3_InitForMetal(shell->Window())) {
    spdlog::error("object_viewer: ImGui_ImplSDL3_InitForMetal failed");
    return 1;
  }
  if (!ImGui_ImplRHI_Init({.device = &device,
                           .compiler = compiler.get(),
                           .target_format = Format::BGRA8Unorm,
                           .framebuffer_width = shell->Width(),
                           .framebuffer_height = shell->Height()})) {
    return 1;
  }

  Camera cam = CameraFor(opt.scene);
  spdlog::info("object_viewer: WASD/QE to move, wheel to zoom, Esc to quit");

  rhi_app::AppShellCallbacks cb;
  cb.OnEvent = [&](const SDL_Event& e) {
    // Stands in for an ImGui widget holding focus: consume EVERYTHING. If
    // Escape still stops the loop, it is because the shell acted on it before
    // asking.
    if (opt.self_test_escape) return true;
    ImGui_ImplSDL3_ProcessEvent(&e);
    // ImGui gets first refusal on input it is using. Without this the camera
    // also acts on a drag inside a panel, which is the two-surfaces confusion
    // the debug/game UI split exists to avoid.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse &&
        (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
      return true;
    }
    if (io.WantCaptureKeyboard && (e.type == SDL_EVENT_KEY_DOWN ||
                                   e.type == SDL_EVENT_KEY_UP)) {
      return true;
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      cam.extent =
          std::clamp(cam.extent * (e.wheel.y > 0 ? 0.9f : 1.1f), 1.0f, 200.0f);
      return true;
    }
    return false;
  };
  cb.OnUpdate = [&](const rhi_app::FrameInfo& f) {
    if (opt.self_test_escape && f.index == 3) {
      SDL_Event esc{};
      esc.type = SDL_EVENT_KEY_DOWN;
      esc.key.scancode = SDL_SCANCODE_ESCAPE;
      SDL_PushEvent(&esc);
    }
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    const float step = cam.extent * std::min(f.dt, 0.1f);
    if (f.keys[SDL_SCANCODE_W]) cam.position.z -= step;
    if (f.keys[SDL_SCANCODE_S]) cam.position.z += step;
    if (f.keys[SDL_SCANCODE_A]) cam.position.x -= step;
    if (f.keys[SDL_SCANCODE_D]) cam.position.x += step;
    if (f.keys[SDL_SCANCODE_E]) cam.position.y += step;
    if (f.keys[SDL_SCANCODE_Q]) cam.position.y -= step;
  };
  cb.OnFrameBegin = [&](uint64_t frame_index) {
    // After BeginFrame, so a SKIPPED frame still recycles its slot.
    if (lines) lines->BeginFrame(frame_index);
    ImGui_ImplRHI_NewFrame(frame_index);
  };
  cb.OnRender = [&](ITextureView* target, const rhi_app::FrameInfo& f) {
    const float aspect = float(f.width) / float(std::max(1u, f.height));
    if (lines) {
      // The SAME scene the headless run of this flag would render, so what is
      // on screen and what a test asserts cannot drift.
      const DebugLineBuffer scene =
          opt.scene == Scene::Lines ? LineScene() : GridScene();
      if (!lines->Upload(scene, cam.View(), cam.Proj(aspect),
                         {float(f.width), float(f.height)}, cam.position)) {
        return false;
      }
    }
    // Rebuilt per frame because the drawable is a different texture each time.
    // Cheap at this size, and the alternative -- caching a graph keyed on a
    // resource that changes every frame -- is how a stale view gets rendered
    // into.
    ImGui_ImplRHI_SetFramebufferSize(f.width, f.height);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("object_viewer");
    ImGui::Text("%u x %u  |  %.1f fps", f.width, f.height,
                f.dt > 0.0f ? 1.0f / f.dt : 0.0f);
    ImGui::Text("camera %.1f %.1f %.1f", cam.position.x, cam.position.y,
                cam.position.z);
    ImGui::SliderFloat("zoom", &cam.extent, 1.0f, 60.0f);
    ImGui::End();
    ImGui::Render();

    RenderGraph graph(device);
    auto out = graph.ImportTexture(target->GetTexture(),
                                   ResourceState::Undefined, "sink");
    if (!out.IsValid()) return false;
    graph.AddRasterPass("clear")
        .ColorTarget(out, LoadOp::Clear, StoreOp::Store, opt.clear)
        .Execute([](const RasterContext&) {});
    if (lines) lines->AddToGraph(graph, out, LoadOp::Load);
    // ImGui LAST, so the debug UI always sits on top -- the same ordering rule
    // the Dawn path had, now expressed as pass order rather than call order.
    ImGui_ImplRHI_AddPass(ImGui::GetDrawData(), graph, out);
    if (!graph.Compile()) return false;
    auto encoder = device.CreateCommandEncoder("frame");
    graph.Execute(*encoder);
    encoder->Finish();
    device.Submit(*encoder);
    return true;
  };

  const auto stats = shell->Run(cb, opt.max_frames);
  if (opt.self_test_escape) {
    // It must have stopped on the Escape, well before the frame cap.
    if (stats.frames_begun >= opt.max_frames) {
      spdlog::error(
          "object_viewer self-test: ran all {} frames -- Escape was swallowed "
          "by the event consumer instead of stopping the loop",
          stats.frames_begun);
      ImGui_ImplRHI_Shutdown();
      ImGui_ImplSDL3_Shutdown();
      ImGui::DestroyContext();
      return 1;
    }
    spdlog::info("object_viewer self-test OK: Escape stopped the loop after {} "
                 "frames", stats.frames_begun);
  }
  ImGui_ImplRHI_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (stats.aborted) return 1;
  if (opt.max_frames > 0 && stats.frames_presented == 0) {
    spdlog::error("object_viewer: ran {} frames and presented none",
                  stats.frames_begun);
    return 1;
  }
  spdlog::info("object_viewer: {} frames, {} presented", stats.frames_begun,
               stats.frames_presented);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, opt)) return 1;

  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true,
                              .label = "object_viewer"});
  if (!device) return 1;

  return opt.headless ? RunHeadless(*device, opt) : RunWindowed(*device, opt);
}
