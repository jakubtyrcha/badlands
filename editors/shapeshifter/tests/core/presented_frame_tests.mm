// The editor's frame, THROUGH RhiRenderer::RenderFrame AND A REAL LAYER, read
// back and looked at.
//
// THIS IS THE GATE THAT WAS MISSING, twice. The headless dump next door renders
// into a texture the test allocates and proves the renderer works; it cannot
// prove the app shows anything, because it never touches a CAMetalLayer, a
// swapchain, a drawable or a colour space. Both times this editor shipped a
// black window, every suite was green:
//
//   * the swapchain was created without ever sizing its layer, so Acquire
//     refused every drawable and nothing recovered;
//   * the app read shaders newer than itself, every pipeline failed, and the
//     renderer came back null.
//
// AND IT DRIVES RenderFrame, which the first version of this file did not. That
// version built its own swapchain so the drawable stayed reachable, and thereby
// skipped the lazy swapchain construction, the deferred resize and the
// colour-space readback -- i.e. exactly the code that produced the first black
// window. It would have passed with that bug reintroduced. RhiRenderer's
// CaptureNextFrame hook exists so the real path can be read instead.
//
// Objective-C++ because constructing the layer IS the test.

#include <catch_amalgamated.hpp>

#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <string>
#include <vector>

#include "badlands_assets.h"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

#include "camera.h"
#include "rhi_renderer.h"
#include "scene.h"
#include "shader_paths.h"

using namespace badlands::rhi;

namespace {

// Inf and NaN decode as themselves, which the first version of this got wrong:
// it fell through to `(e + 112) << 23`, turning every non-finite half into a
// finite 65536.0. That mattered -- an all-NaN frame then satisfied every
// assertion below, because 65536 counts as lit and clamps to 255. This repo had
// just fixed a NaN-in-the-field bug (b7f7ba3), so a garbage frame reading as
// "not blank" was the one blind spot this file could least afford.
float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 31) {
    return m ? std::numeric_limits<float>::quiet_NaN()
             : (s ? -std::numeric_limits<float>::infinity()
                  : std::numeric_limits<float>::infinity());
  }
  if (e == 0) return (s ? -1.f : 1.f) * float(m) / 1024.0f / 16384.0f;
  const uint32_t bits = (s << 31) | ((e + 112) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

TEST_CASE("shapeshifter: the frame RenderFrame presents to a real layer is not blank",
          "[ss-rhi][display]") {
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true,
                              .label = "presented"});
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }

  // The same resolution the app performs, so a shader tree the app could not
  // use is a shader tree this test cannot use either.
  const auto shaders = sq::ResolveShaderLocation();
  REQUIRE(shaders.has_value());
  auto compiler = badlands::slang::CreateSlangCompiler(shaders->search_paths);
  REQUIRE(compiler);

  const uint32_t W = 512, H = 384;
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = 2.0;
  layer.bounds = CGRectMake(0, 0, W / 2.0, H / 2.0);

  auto renderer = sq::RhiRenderer::Create(*device, *compiler, Format::RGBA16Float);
  REQUIRE(renderer);

  // EXACTLY WHAT THE APP DOES, in the app's order: attach before layout (the
  // layer is unsized here, as it is in makeNSView), then a size, then frames.
  // RenderFrame builds the swapchain itself, which is the code under test.
  renderer->AttachLayer((__bridge void*)layer);
  renderer->SetViewportSize(W, H);

  sq::SceneDocument doc;
  sq::Node node;
  node.id = 1;
  node.shape = sq::Shape::Cube;
  doc.add(node);

  sq::Camera camera;
  camera.eye = {2.5f, 2.0f, 3.5f};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fov_y_radians = 1.0472f;
  camera.aspect = float(W) / float(H);

  // A few frames, not one. The first Acquire can legitimately Skip while the
  // drawable pool warms; a WEDGED swapchain skips every frame instead, which is
  // the black-window failure this exists to catch -- so an empty capture after
  // several attempts is a failure, not a reason to pass quietly.
  std::vector<uint8_t> raw;
  for (int i = 0; i < 8 && raw.empty(); ++i) {
    renderer->CaptureNextFrame(&raw);
    renderer->RenderFrame(doc, sq::kInvalidNode, camera);
  }
  REQUIRE_FALSE(raw.empty());
  REQUIRE(raw.size() == size_t(W) * H * 8);

  std::vector<uint8_t> pixels(size_t(W) * H * 4);
  double sum = 0.0;
  size_t lit = 0, nonfinite = 0;
  for (size_t i = 0; i < size_t(W) * H * 4; ++i) {
    uint16_t h;
    std::memcpy(&h, raw.data() + i * 2, 2);
    const float v = HalfToFloat(h);
    if (!std::isfinite(v)) ++nonfinite;
    sum += v;
    if ((i % 4) != 3 && v > 0.05f) ++lit;
    // sRGB-encode for the dump only; the surface itself is extended-linear.
    const float e = v <= 0.0031308f
                        ? v * 12.92f
                        : 1.055f * std::pow(std::max(v, 0.0f), 1.0f / 2.4f) - 0.055f;
    pixels[i] = uint8_t(std::clamp(e, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  // A PNG beside the assertions, because "not blank" failing is a thing you
  // want to LOOK at rather than bisect.
  badlands_write_png("/tmp/shapeshifter_presented.png", pixels.data(), W, H);

  INFO("lit subpixels: " << lit << " of " << (size_t(W) * H * 3));
  INFO("mean channel value: " << sum / double(size_t(W) * H * 4));

  // 0. NOTHING NON-FINITE. A frame of NaN displays as garbage while passing
  //    every "is it bright" test ever written, so it is ruled out first.
  INFO("non-finite channels: " << nonfinite);
  CHECK(nonfinite == 0);

  // 1. NOT THE CLEAR COLOUR EVERYWHERE. The clear is 0.02 -- a frame that drew
  //    nothing at all still fills the surface with it, which is exactly what a
  //    black window looks like.
  CHECK(lit > size_t(W) * H / 100);

  // 2. The ground plate drew. Its grid reaches the bottom edge, where the cube
  //    is not, so this fails if only the raymarch survived.
  size_t edge = 0;
  for (uint32_t x = 0; x < W; ++x) {
    const size_t i = (size_t(H - 4) * W + x) * 4;
    if (pixels[i] > 12 || pixels[i + 1] > 12 || pixels[i + 2] > 14) ++edge;
  }
  INFO("lit texels along the bottom edge: " << edge);
  CHECK(edge > 0);

  // 3. The raymarch drew. The cube sits at the origin under the camera, shaded
  //    by normal, which is saturated in a way no grid line is.
  const size_t mid = (size_t(H / 2) * W + W / 2) * 4;
  const int centre_max =
      std::max({pixels[mid], pixels[mid + 1], pixels[mid + 2]});
  INFO("centre rgb(" << int(pixels[mid]) << "," << int(pixels[mid + 1]) << ","
                     << int(pixels[mid + 2]) << ")");
  CHECK(centre_max > 100);
}

TEST_CASE("shapeshifter: the descriptor decides the surface size, not the layer",
          "[ss-rhi][display]") {
  // THE FIRST BLACK WINDOW, reproduced. MetalSwapchain used to set every layer
  // property except the size, so a layer nobody had sized derived its own
  // drawableSize from bounds * contentsScale at the first nextDrawable. When
  // that disagreed with the descriptor -- which sizes the depth target, the
  // viewport and the projection -- Acquire refused every drawable, Resize()
  // early-returned on the unchanged size, and the surface stayed black for the
  // life of the process.
  //
  // The bounds here DISAGREE with the requested size on purpose. That is what
  // the other cases in this file do not do: they set bounds to exactly
  // size/scale, so the layer's own derivation happens to match and the bug is
  // benign. Comment out the drawableSize assignment in MetalSwapchain's
  // constructor and this case fails while those two still pass -- which is why
  // it exists.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true, .label = "sizing"});
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }
  const auto shaders = sq::ResolveShaderLocation();
  REQUIRE(shaders.has_value());
  auto compiler = badlands::slang::CreateSlangCompiler(shaders->search_paths);
  REQUIRE(compiler);

  const uint32_t W = 512, H = 384;
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = 2.0;
  // 300x200pt at scale 2 derives to 600x400, not the 512x384 asked for below.
  layer.bounds = CGRectMake(0, 0, 300.0, 200.0);

  auto renderer = sq::RhiRenderer::Create(*device, *compiler, Format::RGBA16Float);
  REQUIRE(renderer);
  renderer->AttachLayer((__bridge void*)layer);
  renderer->SetViewportSize(W, H);

  sq::SceneDocument doc;
  sq::Node node;
  node.id = 1;
  node.shape = sq::Shape::Cube;
  doc.add(node);
  sq::Camera camera;
  camera.eye = {2.5f, 2.0f, 3.5f};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fov_y_radians = 1.0472f;
  camera.aspect = float(W) / float(H);

  std::vector<uint8_t> raw;
  for (int i = 0; i < 8 && raw.empty(); ++i) {
    renderer->CaptureNextFrame(&raw);
    renderer->RenderFrame(doc, sq::kInvalidNode, camera);
  }
  // Empty after eight attempts IS the wedge: every Acquire returned Skip.
  INFO("layer drawableSize: " << layer.drawableSize.width << "x"
                              << layer.drawableSize.height);
  REQUIRE_FALSE(raw.empty());
  CHECK(layer.drawableSize.width == Catch::Approx(double(W)));
  CHECK(layer.drawableSize.height == Catch::Approx(double(H)));

  size_t lit = 0;
  for (size_t i = 0; i < size_t(W) * H * 4; ++i) {
    uint16_t hb;
    std::memcpy(&hb, raw.data() + i * 2, 2);
    const float v = HalfToFloat(hb);
    if ((i % 4) != 3 && std::isfinite(v) && v > 0.05f) ++lit;
  }
  CHECK(lit > size_t(W) * H / 100);
}

TEST_CASE("shapeshifter: a resize is followed through to the presented frame",
          "[ss-rhi][display]") {
  // The deferred-resize path in RenderFrame, which nothing else reaches: the
  // size arrives from an AppKit notification at an arbitrary moment and is
  // applied at one point in the frame. Getting it wrong is the OTHER way this
  // window goes black -- Acquire refuses every drawable whose size disagrees
  // with the descriptor, and Resize() early-returns on an unchanged size, so
  // there is no recovery.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true, .label = "resized"});
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }
  const auto shaders = sq::ResolveShaderLocation();
  REQUIRE(shaders.has_value());
  auto compiler = badlands::slang::CreateSlangCompiler(shaders->search_paths);
  REQUIRE(compiler);

  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = 2.0;
  auto renderer = sq::RhiRenderer::Create(*device, *compiler, Format::RGBA16Float);
  REQUIRE(renderer);
  renderer->AttachLayer((__bridge void*)layer);

  sq::SceneDocument doc;
  sq::Node node;
  node.id = 1;
  node.shape = sq::Shape::Cube;
  doc.add(node);
  sq::Camera camera;
  camera.eye = {2.5f, 2.0f, 3.5f};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fov_y_radians = 1.0472f;

  // A fractional point size, which is what a SwiftUI split view produces, at
  // two different sizes -- the second exercises Resize rather than creation.
  for (const auto [w, h] : {std::pair<uint32_t, uint32_t>{400, 300},
                            std::pair<uint32_t, uint32_t>{513, 301}}) {
    layer.bounds = CGRectMake(0, 0, w / 2.0, h / 2.0);
    renderer->SetViewportSize(w, h);
    camera.aspect = float(w) / float(h);

    std::vector<uint8_t> raw;
    for (int i = 0; i < 8 && raw.empty(); ++i) {
      renderer->CaptureNextFrame(&raw);
      renderer->RenderFrame(doc, sq::kInvalidNode, camera);
    }
    INFO("size " << w << "x" << h);
    REQUIRE(raw.size() == size_t(w) * h * 8);

    size_t lit = 0;
    for (size_t i = 0; i < size_t(w) * h * 4; ++i) {
      uint16_t hb;
      std::memcpy(&hb, raw.data() + i * 2, 2);
      const float v = HalfToFloat(hb);
      if ((i % 4) != 3 && std::isfinite(v) && v > 0.05f) ++lit;
    }
    INFO("lit subpixels: " << lit);
    CHECK(lit > size_t(w) * h / 100);
  }
}
