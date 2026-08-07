// The app layer's own tests: argument parsing and frame timing.
//
// Neither needs a GPU, a window or a display, which is the point -- these were
// the two pieces with no coverage at all, and one of them shipped a defect
// (--frames -1 accepted as 18446744073709551615) that was caught only because
// object_viewer happened to have a ctest entry asserting its OLD parser
// rejected it. That is coverage by coincidence.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <glm/gtc/packing.hpp>
#include <string>
#include <vector>

#include "engine/app/rhi_app.hpp"

using badlands::rhi_app::FrameClock;
using badlands::rhi_app::ParseAppArgs;
using badlands::rhi_app::RhiAppOptions;

namespace {

// ParseAppArgs mutates argv in place, so each case needs its own writable copy.
struct Args {
  std::vector<std::string> storage;
  std::vector<char*> pointers;
  int argc = 0;

  explicit Args(std::initializer_list<const char*> in) {
    for (const char* a : in) storage.emplace_back(a);
    for (auto& s : storage) pointers.push_back(s.data());
    argc = int(pointers.size());
  }
  RhiAppOptions Parse() { return ParseAppArgs(argc, pointers.data()); }
  // What was LEFT for the app's own parser.
  std::vector<std::string> Remaining() const {
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.emplace_back(pointers[size_t(i)]);
    return out;
  }
};

}  // namespace

TEST_CASE("app args: the layer's flags are consumed and removed", "[rhiapp]") {
  // REMOVED, not merely read. A flag left in argv reaches the app's own parser
  // as an unknown argument and is refused there -- which reads as the app
  // rejecting a flag the layer documents, and is exactly what happened the
  // first time object_viewer ran with --fixed-dt.
  Args a{"prog", "--frames", "12", "--scene", "spheres", "--fixed-dt", "0.5",
         "--screenshot", "/tmp/x.png", "--debug-view", "lit"};
  const auto opt = a.Parse();
  REQUIRE(opt.valid);
  CHECK(opt.max_frames == 12);
  CHECK(opt.fixed_dt == Catch::Approx(0.5f));
  CHECK(opt.screenshot_path == "/tmp/x.png");

  // The app's flags survive, in order, with argv[0] intact.
  const auto rest = a.Remaining();
  REQUIRE(rest.size() == 5);
  CHECK(rest[0] == "prog");
  CHECK(rest[1] == "--scene");
  CHECK(rest[2] == "spheres");
  CHECK(rest[3] == "--debug-view");
  CHECK(rest[4] == "lit");
}

TEST_CASE("app args: nothing is consumed when nothing is ours", "[rhiapp]") {
  Args a{"prog", "--scene", "plane", "--headless"};
  const auto opt = a.Parse();
  CHECK(opt.valid);
  CHECK(opt.max_frames == 0);
  CHECK(opt.fixed_dt == 0.0f);
  CHECK(opt.screenshot_path.empty());
  CHECK(a.Remaining().size() == 4);
}

TEST_CASE("app args: a negative frame count is refused", "[rhiapp]") {
  // THE DEFECT THIS FILE EXISTS FOR. strtoull accepts a leading minus and
  // NEGATES it, so "-1" parses successfully as 18446744073709551615 -- a frame
  // cap that silently means "forever" instead of being rejected.
  Args a{"prog", "--frames", "-1"};
  CHECK_FALSE(a.Parse().valid);

  Args b{"prog", "--frames", "-0"};
  CHECK_FALSE(b.Parse().valid);
}

TEST_CASE("app args: malformed values are refused, not coerced", "[rhiapp]") {
  // Each of these has a plausible wrong answer: strtoull/strtod stop at the
  // first bad character and report success for the prefix, so "12abc" would
  // become 12 and "1.5.5" would become 1.5.
  Args trailing{"prog", "--frames", "12abc"};
  CHECK_FALSE(trailing.Parse().valid);

  Args word{"prog", "--frames", "many"};
  CHECK_FALSE(word.Parse().valid);

  Args dt_trailing{"prog", "--fixed-dt", "1.5.5"};
  CHECK_FALSE(dt_trailing.Parse().valid);

  Args dt_word{"prog", "--fixed-dt", "fast"};
  CHECK_FALSE(dt_word.Parse().valid);

  // A fixed step of zero is not "wall clock" -- it is a request the caller
  // spelled out, and honouring it as its opposite is one value meaning two
  // things.
  Args dt_zero{"prog", "--fixed-dt", "0"};
  CHECK_FALSE(dt_zero.Parse().valid);

  Args dt_neg{"prog", "--fixed-dt", "-0.5"};
  CHECK_FALSE(dt_neg.Parse().valid);
}

TEST_CASE("app args: a flag with no value is refused", "[rhiapp]") {
  // Reading past the end here would be a buffer overrun, not a wrong number.
  Args frames{"prog", "--frames"};
  CHECK_FALSE(frames.Parse().valid);

  Args dt{"prog", "--fixed-dt"};
  CHECK_FALSE(dt.Parse().valid);

  Args shot{"prog", "--screenshot"};
  CHECK_FALSE(shot.Parse().valid);
}

TEST_CASE("frame clock: a fixed step ignores the measured time", "[rhiapp]") {
  // THE WHOLE POINT OF --fixed-dt. What the loop measured is discarded, so the
  // same frame index means the same simulated time on any machine -- which is
  // what makes a capture reproducible and "N frames from start" meaningful.
  FrameClock clock{.fixed_dt = 0.02f};
  const auto a = clock.Advance(/*real_dt=*/0.9f, 0, nullptr);
  CHECK(a.real_dt == Catch::Approx(0.02f));
  CHECK(a.elapsed == Catch::Approx(0.02));

  const auto b = clock.Advance(/*real_dt=*/0.0001f, 1, nullptr);
  CHECK(b.real_dt == Catch::Approx(0.02f));
  CHECK(b.elapsed == Catch::Approx(0.04));
}

TEST_CASE("frame clock: wall time passes through untouched", "[rhiapp]") {
  FrameClock clock;  // fixed_dt = 0
  const auto a = clock.Advance(0.011f, 0, nullptr);
  CHECK(a.real_dt == Catch::Approx(0.011f));
  const auto b = clock.Advance(0.023f, 1, nullptr);
  CHECK(b.real_dt == Catch::Approx(0.023f));
  CHECK(b.elapsed == Catch::Approx(0.034));
}

TEST_CASE("frame clock: fixed steps accumulate rather than round", "[rhiapp]") {
  // kTickDt is 1/30. A frame shorter than one tick yields NO steps but must not
  // lose the time -- three of them add up to one step. A clock that rounded per
  // frame would drop it, and the simulation would run slow by an amount that
  // varies with the frame rate.
  FrameClock clock{.fixed_dt = 1.0f / 90.0f};
  CHECK(clock.Advance(0.0f, 0, nullptr).fixed_steps == 0);
  CHECK(clock.Advance(0.0f, 1, nullptr).fixed_steps == 0);
  CHECK(clock.Advance(0.0f, 2, nullptr).fixed_steps == 1);

  // And a frame worth several ticks yields several.
  FrameClock fast{.fixed_dt = 0.1f};  // 3 ticks of 1/30 per frame
  CHECK(fast.Advance(0.0f, 0, nullptr).fixed_steps == 3);
}

TEST_CASE("frame clock: a stall cannot detonate the step count", "[rhiapp]") {
  // A breakpoint or a slow first frame otherwise asks for hundreds of steps at
  // once, and an app that simulates each spends longer than the stall did --
  // the spiral kMaxSimTicksPerFrame exists to stop.
  FrameClock clock;
  const auto t = clock.Advance(/*real_dt=*/600.0f, 0, nullptr);
  CHECK(t.fixed_steps == uint32_t(badlands::kMaxSimTicksPerFrame));

  // And the REMAINDER IS DROPPED rather than carried: catching up on time
  // nobody was watching just moves the spiral to the next frame.
  const auto next = clock.Advance(0.0f, 1, nullptr);
  CHECK(next.fixed_steps == 0);
}

TEST_CASE("frame clock: index and keys pass through", "[rhiapp]") {
  FrameClock clock;
  const bool keys[4] = {false, true, false, false};
  const auto t = clock.Advance(0.016f, 42, keys);
  CHECK(t.index == 42);
  CHECK(t.keys == keys);
}

// --- The UI composite ------------------------------------------------------
//
// THE PIECE src/engine/CLAUDE.md RECORDS AS ALREADY GOT WRONG ONCE, and the
// reason UiCompositor was extracted from the run loop at all: while it lived
// inline, the only way to check it was to open a window and look at a
// translucent panel -- on an SDR display, where the defect is invisible.
//
// Needs a Metal device and a Slang SDK; skipped where either is absent.

#include "engine/app/ui_compositor.hpp"
#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"

namespace rhi = badlands::rhi;
using badlands::rhi_app::UiCompositor;

namespace {

std::unique_ptr<badlands::slang::SlangCompiler> MakeAppCompiler() {
  const std::vector<std::string> paths = {"shaders/slang/common",
                                          "shaders/slang/app"};
  return badlands::slang::CreateSlangCompiler(paths);
}

// Composites a known overlay texel over a known surface and returns the result.
// `overlay` is ENCODED and PREMULTIPLIED, as ImGui leaves it.
struct CompositeResult {
  bool ran = false;
  float r = 0.0f, g = 0.0f, b = 0.0f;
};

CompositeResult RunComposite(rhi::Format surface_format,
                             rhi::ColorSpace surface_cs,
                             const uint8_t overlay_rgba[4],
                             const float surface_clear[4]) {
  CompositeResult out;
  auto device = rhi::CreateDevice({.backend = rhi::BackendKind::Metal,
                                   .enable_validation = false,
                                   .label = "ui_composite_tests"});
  if (!device) return out;
  auto compiler = MakeAppCompiler();
  if (!compiler) return out;

  constexpr uint32_t kSize = 4;
  auto compositor = UiCompositor::Create(*device, *compiler, surface_format,
                                         surface_cs, kSize, kSize);
  REQUIRE(compositor);

  auto surface = device->CreateTexture({.width = kSize, .height = kSize,
                                        .format = surface_format,
                                        .usage = rhi::TextureUsage::RenderTarget |
                                                 rhi::TextureUsage::CopySrc,
                                        .label = "surface"});
  REQUIRE(surface);

  // The overlay is filled directly rather than by ImGui: this is a test of the
  // COMPOSITE, and a draw list would put ImGui's rasteriser between the value
  // under test and the assertion.
  std::vector<uint8_t> texels(size_t(kSize) * kSize * 4);
  for (size_t i = 0; i < texels.size(); i += 4) {
    texels[i + 0] = overlay_rgba[0];
    texels[i + 1] = overlay_rgba[1];
    texels[i + 2] = overlay_rgba[2];
    texels[i + 3] = overlay_rgba[3];
  }
  compositor->Overlay()->Write(0, 0, {texels.data(), texels.size()});

  device->BeginFrame();
  badlands::graph::RenderGraph graph(*device);
  auto surface_h = graph.ImportTexture(surface.get(),
                                       rhi::ResourceState::Undefined, "surface");
  auto ui_h = graph.ImportTexture(compositor->Overlay(),
                                  rhi::ResourceState::Undefined, "ui");
  REQUIRE(surface_h.IsValid());
  REQUIRE(ui_h.IsValid());
  // The surface is cleared to a known value FIRST, so the composite has
  // something to blend against and the result is a function of both.
  graph.AddRasterPass("surface_clear")
      .ColorTarget(surface_h, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                   surface_clear)
      .Execute([](const badlands::graph::RasterContext&) {});
  REQUIRE(compositor->Composite(graph, ui_h, surface_h));
  REQUIRE(graph.Compile());

  auto encoder = device->CreateCommandEncoder("composite");
  graph.Execute(*encoder);
  encoder->Transition(surface.get(), rhi::ResourceState::CopySrc);
  auto* view = surface->CreateView({.mip_count = 1, .layer_count = 1});
  REQUIRE(view);
  auto rb = device->ReadTexture(*encoder, view);
  REQUIRE(rb);
  encoder->Finish();
  device->Submit(*encoder);
  REQUIRE(rb->Wait());

  const auto data = rb->Data();
  REQUIRE(data.size() >= 8);
  if (surface_format == rhi::Format::RGBA16Float) {
    const uint16_t* h = reinterpret_cast<const uint16_t*>(data.data());
    out.r = glm::unpackHalf1x16(h[0]);
    out.g = glm::unpackHalf1x16(h[1]);
    out.b = glm::unpackHalf1x16(h[2]);
  } else {
    out.r = float(data[0]) / 255.0f;
    out.g = float(data[1]) / 255.0f;
    out.b = float(data[2]) / 255.0f;
  }
  out.ran = true;
  device->EndFrame();
  return out;
}

}  // namespace

TEST_CASE("ui composite: an opaque overlay replaces the surface", "[rhiapp][gpu]") {
  // alpha 1: the two forms must AGREE here, and both must simply show the
  // overlay. This is the endpoint that catches a blend state that is not
  // premultiplied at all.
  const uint8_t overlay[4] = {200, 100, 50, 255};
  const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  const auto srgb = RunComposite(rhi::Format::RGBA8Unorm,
                                 rhi::ColorSpace::Srgb, overlay, clear);
  if (!srgb.ran) return;  // no Metal or no Slang here
  CHECK(srgb.r == Catch::Approx(200.0f / 255.0f).margin(0.01));
  CHECK(srgb.g == Catch::Approx(100.0f / 255.0f).margin(0.01));
}

TEST_CASE("ui composite: a transparent overlay leaves the surface alone",
          "[rhiapp][gpu]") {
  // alpha 0 AND colour 0, which is what a cleared overlay holds over most of
  // the screen. Premultiplied means the surface must come back untouched.
  const uint8_t overlay[4] = {0, 0, 0, 0};
  const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};

  const auto srgb = RunComposite(rhi::Format::RGBA8Unorm,
                                 rhi::ColorSpace::Srgb, overlay, clear);
  if (!srgb.ran) return;
  CHECK(srgb.r == Catch::Approx(0.25f).margin(0.01));
  CHECK(srgb.g == Catch::Approx(0.5f).margin(0.01));
  CHECK(srgb.b == Catch::Approx(0.75f).margin(0.01));
}

TEST_CASE("ui composite: an extended-range surface gets LINEAR overlay colour",
          "[rhiapp][gpu]") {
  // THE DIFFERENCE BETWEEN THE TWO FORMS, isolated.
  //
  // My first attempt at this test asserted that a faint fringe over a bright
  // scene kept its headroom -- and PASSED with the linear branch deleted,
  // because this composite never touches the scene: hardware blending does
  // dst*(1-srcAlpha), which cannot clamp a float target. The clamping the
  // CLAUDE.md note describes comes from object_viewer's output pass encoding
  // the SCENE, which this does not do.
  //
  // What the two forms actually disagree about is the OVERLAY's own colour. An
  // opaque mid-grey panel is authored as the encoded byte 128; written into a
  // linear surface it must arrive as ~0.216, not 0.502. Alpha 1 so the surface
  // contributes nothing and the assertion is about the overlay alone.
  const uint8_t grey[4] = {128, 128, 128, 255};
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  const auto edr = RunComposite(rhi::Format::RGBA16Float,
                                rhi::ColorSpace::ExtendedLinearDisplayP3,
                                grey, black);
  if (!edr.ran) return;
  INFO("extended-range grey -> " << edr.r << " (encoded form would give 0.502)");
  CHECK(edr.r == Catch::Approx(0.216f).margin(0.02));

  // And the SAME overlay on an 8-bit surface must stay encoded, or the panel
  // comes back far too dark. The two forms agreeing here would mean one of
  // them is wrong.
  const auto srgb = RunComposite(rhi::Format::RGBA8Unorm,
                                 rhi::ColorSpace::Srgb, grey, black);
  REQUIRE(srgb.ran);
  INFO("8-bit grey -> " << srgb.r);
  CHECK(srgb.r == Catch::Approx(128.0f / 255.0f).margin(0.02));
}

TEST_CASE("ui composite: a half-alpha panel lands where it was authored",
          "[rhiapp][gpu]") {
  // The case the whole encoded-overlay rule exists for: 50% alpha over black.
  // Premultiplied white at half alpha is (128,128,128,128) encoded.
  const uint8_t panel[4] = {128, 128, 128, 128};
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  const auto srgb = RunComposite(rhi::Format::RGBA8Unorm,
                                 rhi::ColorSpace::Srgb, panel, black);
  if (!srgb.ran) return;
  INFO("half-alpha over black -> " << srgb.r);
  // Over black the premultiplied colour passes straight through, so the byte
  // must come back as the byte that was authored -- NOT 0.735, which is what
  // blending it in linear space produces.
  CHECK(srgb.r == Catch::Approx(128.0f / 255.0f).margin(0.02));
}
