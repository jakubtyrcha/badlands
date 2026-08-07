// A swapchain over a REAL CAMetalLayer, which the rest of badlands_rhi_metal_tests
// deliberately avoids: every other case there is headless, and the layer path is
// exactly where the headless ones cannot look.
//
// Objective-C++ rather than C++ for that reason alone -- constructing the layer
// is the test. It stays in this target rather than growing a windowed one,
// because a CAMetalLayer needs no window, no display and no run loop: it vends
// drawables offscreen, which is what makes this deterministic under ctest.

#include <catch_amalgamated.hpp>

#import <QuartzCore/CAMetalLayer.h>

#include "engine/rhi/metal/metal_rhi.hpp"

using namespace badlands::rhi;

namespace {

std::unique_ptr<IRhiDevice> MakeMetal() {
  return CreateDevice({.backend = BackendKind::Metal,
                       .enable_validation = true,
                       .label = "metal_layer_tests"});
}

}  // namespace

TEST_CASE("metal: a layer-backed swapchain is sized at creation, not first resize",
          "[rhi][metal][swapchain]") {
  // The swapchain used to set every layer property EXCEPT the size: device,
  // pixelFormat, framebufferOnly, maximumDrawableCount, displaySyncEnabled --
  // and then leave drawableSize to whatever the layer already had. Only
  // Resize() ever wrote it, so a swapchain created and never resized ran
  // against a layer that disagreed with its own descriptor.
  //
  // Nothing recovers from that. Acquire refuses every drawable whose size does
  // not match the descriptor, and Resize() early-returns when the caller's size
  // has not changed -- which it has not, because the caller asked for this size
  // at creation. The viewport stays black for the life of the process.
  //
  // The bounds here are the case that found it: 1200.4pt is what a SwiftUI
  // split view hands over, and 1200.4 * 2 narrows to 2400 device pixels.
  auto device = MakeMetal();
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }

  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = 2.0;
  layer.bounds = CGRectMake(0, 0, 1200.4, 400.0);
  // A layer nobody has sized reports 0x0 no matter what its bounds say: the
  // derivation from bounds * contentsScale happens at the first nextDrawable,
  // not on assignment. That is precisely the problem being fixed -- left at
  // 0x0, the LAYER picks the surface size later and the descriptor only finds
  // out by having its drawables refused.
  REQUIRE(layer.drawableSize.width == Catch::Approx(0.0));

  const uint32_t width = 2400;  // uint32_t(1200.4 * 2.0), the caller's narrowing
  const uint32_t height = 800;
  auto sc = device->CreateSwapchain({.native_window = (__bridge void*)layer,
                                     .width = width,
                                     .height = height,
                                     .format = Format::BGRA8Unorm,
                                     .vsync = false,
                                     .label = "fractional"});
  REQUIRE(sc);

  CHECK(layer.drawableSize.width == Catch::Approx(double(width)));
  CHECK(layer.drawableSize.height == Catch::Approx(double(height)));

  // And the consequence, which is the part worth having: a drawable arrives at
  // the size the descriptor asked for, so the frame renders.
  device->BeginFrame();
  const AcquiredFrame frame = sc->Acquire();
  CHECK(frame.status == AcquireStatus::Ok);
  CHECK(frame.view != nullptr);
  if (frame.status == AcquireStatus::Ok) {
    sc->Present();
  }
  device->EndFrame();
  device->WaitIdle();
}

TEST_CASE("metal: a layer-backed swapchain follows a resize", "[rhi][metal][swapchain]") {
  // The other half of the same property. Resize() already set drawableSize; this
  // pins that the constructor's new assignment did not make the two disagree
  // about which of them owns the layer's size.
  auto device = MakeMetal();
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }

  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = 2.0;
  layer.bounds = CGRectMake(0, 0, 320.0, 200.0);
  auto sc = device->CreateSwapchain({.native_window = (__bridge void*)layer,
                                     .width = 640,
                                     .height = 400,
                                     .format = Format::BGRA8Unorm,
                                     .vsync = false,
                                     .label = "resized"});
  REQUIRE(sc);

  sc->Resize(700, 300);
  CHECK(sc->GetWidth() == 700);
  CHECK(sc->GetHeight() == 300);
  CHECK(layer.drawableSize.width == Catch::Approx(700.0));
  CHECK(layer.drawableSize.height == Catch::Approx(300.0));

  device->BeginFrame();
  const AcquiredFrame frame = sc->Acquire();
  CHECK(frame.status == AcquireStatus::Ok);
  if (frame.status == AcquireStatus::Ok) {
    sc->Present();
  }
  device->EndFrame();
  device->WaitIdle();
}
