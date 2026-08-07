// The editor's frame, THROUGH A REAL LAYER, read back and looked at.
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
// Neither is visible from a texture. Both are visible from here.
//
// Objective-C++ because constructing the layer IS the test.

#include <catch_amalgamated.hpp>

#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "badlands_assets.h"
#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

#include "camera.h"
#include "rhi_renderer.h"
#include "scene.h"
#include "shader_paths.h"

using namespace badlands::rhi;

namespace {

float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 0) return (s ? -1.f : 1.f) * float(m) / 1024.0f / 16384.0f;
  const uint32_t bits = (s << 31) | ((e + 112) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

TEST_CASE("shapeshifter: a frame presented through a real layer is not blank",
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

  // The swapchain is built here rather than left to RenderFrame so the drawable
  // stays reachable for readback. Everything that matters is still the real
  // thing: a real layer, the real swapchain (so drawableSize and the colour
  // space tag are exercised), the real pipelines, the real frame graph.
  auto swapchain = device->CreateSwapchain(
      {.native_window = (__bridge void*)layer,
       .width = W,
       .height = H,
       .format = Format::RGBA16Float,
       // Matches the app. RGBA16Float on a real layer is refused without it,
       // so this doubles as a check that the editor's presentation contract is
       // still constructible.
       .color_space = ColorSpace::ExtendedLinearDisplayP3,
       .vsync = false,
       .label = "presented"});
  REQUIRE(swapchain);

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

  auto readback = device->CreateBuffer({.size = uint64_t(W) * H * 8,
                                        .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
                                        .label = "presented_rb"});
  auto depth_tex = device->CreateTexture({.width = W, .height = H,
                                          .format = Format::Depth32Float,
                                          .usage = TextureUsage::DepthStencil,
                                          .label = "presented_depth"});
  REQUIRE(readback);
  REQUIRE(depth_tex);

  // A few frames, not one. The first Acquire can legitimately Skip while the
  // drawable pool warms; a swapchain that is WEDGED skips every frame instead,
  // which is the black-window failure this catches.
  bool drew = false;
  for (int i = 0; i < 4 && !drew; ++i) {
    device->BeginFrame();
    renderer->BeginFrame(device->CurrentFrame());
    const AcquiredFrame acquired = swapchain->Acquire();
    if (acquired.status != AcquireStatus::Ok) {
      device->EndFrame();
      continue;
    }

    badlands::graph::RenderGraph g(*device);
    auto color = g.ImportTexture(acquired.view->GetTexture(),
                                 ResourceState::Undefined, "backbuffer");
    auto depth = g.ImportTexture(depth_tex.get(), ResourceState::Undefined, "depth");
    REQUIRE(renderer->BuildFrame(g, color, depth, doc, sq::kInvalidNode, camera, W, H));
    REQUIRE(g.Compile());

    auto encoder = device->CreateCommandEncoder("presented");
    g.Execute(*encoder);
    // Copied BEFORE Present, while the drawable is still ours. framebufferOnly
    // is NO on this layer (the swapchain sets it), so the copy is legal.
    encoder->Transition(acquired.view->GetTexture(), ResourceState::CopySrc);
    encoder->Transition(readback.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(acquired.view->GetTexture(), 0, 0, readback.get(), 0);
    encoder->Finish();
    device->Submit(*encoder);
    swapchain->Present();
    device->EndFrame();
    device->WaitIdle();
    drew = true;
  }
  // Four consecutive skips is the wedge, and it is a failure rather than a
  // reason to pass quietly.
  REQUIRE(drew);

  std::vector<uint8_t> raw(size_t(W) * H * 8);
  REQUIRE(readback->Read(0, raw));

  std::vector<uint8_t> pixels(size_t(W) * H * 4);
  double sum = 0.0;
  size_t lit = 0;
  for (size_t i = 0; i < size_t(W) * H * 4; ++i) {
    uint16_t h;
    std::memcpy(&h, raw.data() + i * 2, 2);
    const float v = HalfToFloat(h);
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
