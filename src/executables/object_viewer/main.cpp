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

#include "badlands_assets.h"
#include "engine/app/rhi_app_shell.hpp"
#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"

using namespace badlands;
using namespace badlands::rhi;
using badlands::graph::RasterContext;
using badlands::graph::RenderGraph;

namespace {

struct Options {
  bool headless = false;
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

// THE GRAPH, built identically for both modes. `sink` is whatever the caller
// wants rendered into; the graph neither knows nor cares whether a display is
// attached.
bool BuildGraph(RenderGraph& graph, ITexture* sink, const Options& opt) {
  // Undefined on entry every frame: a freshly acquired drawable is a NEW
  // resource each time, so any state carried over from last frame would be a
  // lie. Stating it here rather than assuming it is what the entry-state
  // parameter is for.
  auto out = graph.ImportTexture(sink, ResourceState::Undefined, "sink");
  if (!out.IsValid()) return false;

  graph.AddRasterPass("clear")
      .ColorTarget(out, LoadOp::Clear, StoreOp::Store, opt.clear)
      .Execute([](const RasterContext&) {
        // Stage 1 draws nothing. The clear IS the frame, and the pass exists so
        // the graph has a real target to derive a transition for.
      });
  return graph.Compile();
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

  RenderGraph graph(device);
  if (!BuildGraph(graph, sink.get(), opt)) return 1;

  device.BeginValidationScope();
  device.BeginFrame();
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
  // THE ASSERTION, and the reason the headless ctest means anything: every
  // texel must be the clear colour the graph was given. Writing a PNG and
  // exiting 0 would pass just as well against a graph that recorded no pass at
  // all, or one whose clear was ignored -- both of which produce a plausible
  // file. Exit status IS the check; there is no test framework around this.
  auto expect = [](float v) {
    return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t want[4] = {expect(opt.clear[0]), expect(opt.clear[1]),
                           expect(opt.clear[2]), expect(opt.clear[3])};
  for (size_t i = 0; i < pixels.size(); i += 4) {
    for (int c = 0; c < 4; ++c) {
      // +/- 1 LSB: the clear happens in float and rounds once.
      if (std::abs(int(pixels[i + c]) - int(want[c])) > 1) {
        spdlog::error(
            "object_viewer: texel {} is rgba({},{},{},{}) but the graph was "
            "asked to clear to rgba({},{},{},{})",
            i / 4, pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3],
            want[0], want[1], want[2], want[3]);
        return 1;
      }
    }
  }

  badlands_write_png(opt.out.c_str(), pixels.data(), opt.width, opt.height);
  spdlog::info("object_viewer: wrote {} ({}x{}), every texel rgba({},{},{},{})",
               opt.out, opt.width, opt.height, want[0], want[1], want[2],
               want[3]);
  return 0;
}

int RunWindowed(IRhiDevice& device, const Options& opt) {
  auto shell = rhi_app::AppShell::Create(device, {.title = "badlands object_viewer",
                                                  .width = opt.width,
                                                  .height = opt.height,
                                                  .present_format =
                                                      Format::BGRA8Unorm});
  if (!shell) return 1;
  spdlog::info("object_viewer: Esc to quit");

  rhi_app::AppShellCallbacks cb;
  cb.OnRender = [&](ITextureView* target, const rhi_app::FrameInfo&) {
    // Rebuilt per frame because the drawable is a different texture each time.
    // Cheap at this size, and the alternative -- caching a graph keyed on a
    // resource that changes every frame -- is how a stale view gets rendered
    // into.
    RenderGraph graph(device);
    if (!BuildGraph(graph, target->GetTexture(), opt)) return false;
    auto encoder = device.CreateCommandEncoder("frame");
    graph.Execute(*encoder);
    encoder->Finish();
    device.Submit(*encoder);
    return true;
  };

  const auto stats = shell->Run(cb, opt.max_frames);
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
