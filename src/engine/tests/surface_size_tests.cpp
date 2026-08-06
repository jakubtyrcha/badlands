// The resize decision, without a window.
//
// The render loop's bug was invisible to the windowed self-test: it gated the
// swapchain resize on the size of the render TARGETS, which are deliberately
// not rebuilt at zero size. Minimize and restore to the same size, and the
// loop saw "nothing changed" and left the swapchain at 0x0 forever. Only a
// test that plays that exact sequence catches it, so the decision is a pure
// function and this is that sequence.

#include <catch_amalgamated.hpp>

#include "engine/app/surface_size.hpp"

using badlands::rhi_app::SurfaceSizeTracker;

TEST_CASE("surface size: no change asks for nothing", "[rhi_lab]") {
  SurfaceSizeTracker t{800, 600};
  const auto a = t.Update(800, 600);
  CHECK_FALSE(a.resize_swapchain);
  CHECK_FALSE(a.rebuild_targets);
}

TEST_CASE("surface size: an ordinary resize rebuilds everything", "[rhi_lab]") {
  SurfaceSizeTracker t{800, 600};
  const auto a = t.Update(1024, 768);
  CHECK(a.resize_swapchain);
  CHECK(a.rebuild_targets);
  CHECK(t.width() == 1024);
  CHECK(t.height() == 768);
}

TEST_CASE("surface size: zero resizes the swapchain but builds no targets",
          "[rhi_lab]") {
  // A minimized window. The swapchain must hear about it -- so Acquire starts
  // reporting Skip -- but a 0x0 texture is not a thing.
  SurfaceSizeTracker t{800, 600};
  const auto a = t.Update(0, 0);
  CHECK(a.resize_swapchain);
  CHECK_FALSE(a.rebuild_targets);
}

TEST_CASE("surface size: restoring to the SAME size still resizes",
          "[rhi_lab]") {
  // The regression. Gating on the targets -- which kept 800x600 through the
  // minimize because they are not rebuilt at zero -- made this third step look
  // like "no change", so the swapchain was never told to come back from 0x0
  // and the restored window rendered nothing at all.
  SurfaceSizeTracker t{800, 600};
  REQUIRE(t.Update(0, 0).resize_swapchain);

  const auto back = t.Update(800, 600);
  CHECK(back.resize_swapchain);
  CHECK(back.rebuild_targets);
  CHECK(t.width() == 800);
  CHECK(t.height() == 600);
}

TEST_CASE("surface size: a live drag coalesces to the last size", "[rhi_lab]") {
  // Only the frame's final pending size matters; intermediate ones never reach
  // the tracker, because the loop overwrites pending_w/h as events arrive.
  SurfaceSizeTracker t{800, 600};
  CHECK(t.Update(900, 700).resize_swapchain);
  CHECK_FALSE(t.Update(900, 700).resize_swapchain);
}
