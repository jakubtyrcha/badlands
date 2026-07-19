// Pure-CPU tests for GameCameraController's zoom.
//
// The headline property is ZoomAtCursor's contract: whatever ground point sits
// under the cursor must STAY under it across the zoom. That is the whole reason
// cursor-anchored zoom feels right, and it is invisible to any test that only
// checks the height changed — so it gets checked directly, off-centre (a
// centre-pixel check passes even with the anchoring removed entirely, since the
// centre is the focus point and never moves).

#include <catch_amalgamated.hpp>

#include <cmath>

#include <glm/glm.hpp>

#include "engine/app/game_camera_controller.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/core/ray.hpp"

using namespace badlands;

namespace {

constexpr float kW = 1600.0f;
constexpr float kH = 900.0f;
const glm::vec2 kScreen(kW, kH);

GameCameraController MakeController() {
  GameCameraController gc;
  gc.focus = glm::vec3(100.0f, 0.0f, 250.0f);
  gc.pitch_deg = 55.0f;
  gc.height = 60.0f;
  gc.min_height = 5.0f;
  gc.max_height = 400.0f;
  return gc;
}

Camera MakeCamera(const GameCameraController& gc) {
  Camera c;
  c.fov = 45.0f;
  c.aspect = kW / kH;
  gc.UpdateCamera(c);
  return c;
}

// The ground point currently under `pixel`.
glm::vec3 GroundUnder(const Camera& cam, glm::vec2 pixel) {
  glm::vec3 hit;
  REQUIRE(IntersectGroundPlane(ScreenPointToRay(cam, pixel, kScreen), 0.0f, hit));
  return hit;
}

}  // namespace

TEST_CASE("Zoom: multiplicative, symmetric, and clamped") {
  GameCameraController gc = MakeController();
  const float start = gc.height;

  // 1/zoom_speed notches halve the height.
  gc.Zoom(1.0f / gc.zoom_speed);
  CHECK(gc.height == Catch::Approx(start * 0.5f).margin(1e-3));

  // Exactly reversible — the `height *= (1 - n*speed)` form is not.
  gc.Zoom(-1.0f / gc.zoom_speed);
  CHECK(gc.height == Catch::Approx(start).margin(1e-3));

  // Positive notches (scroll away) move toward the ground.
  gc.Zoom(1.0f);
  CHECK(gc.height < start);

  // Clamped both ways, and never <= 0 no matter how violent the input.
  gc = MakeController();
  gc.Zoom(1e6f);
  CHECK(gc.height == Catch::Approx(gc.min_height));
  CHECK(gc.height > 0.0f);
  gc.Zoom(-1e6f);
  CHECK(gc.height == Catch::Approx(gc.max_height));
}

TEST_CASE("ZoomAtCursor: the ground point under the cursor stays put") {
  // Off-centre on BOTH axes: the centre pixel is the focus point and holds
  // still even with no anchoring at all, so it proves nothing.
  const glm::vec2 pixel(kW * 0.78f, kH * 0.28f);

  GameCameraController gc = MakeController();
  Camera cam = MakeCamera(gc);
  const glm::vec3 before = GroundUnder(cam, pixel);

  ZoomAtCursor(gc, cam, 2.0f, pixel, kScreen);

  const glm::vec3 after = GroundUnder(cam, pixel);
  INFO("before = " << before.x << "," << before.z
                   << "  after = " << after.x << "," << after.z);
  CHECK(after.x == Catch::Approx(before.x).margin(1e-2));
  CHECK(after.z == Catch::Approx(before.z).margin(1e-2));
  CHECK(gc.height < 60.0f);  // and it actually zoomed
}

TEST_CASE("ZoomAtCursor: anchor holds zooming out, and round-trips") {
  const glm::vec2 pixel(kW * 0.2f, kH * 0.75f);

  GameCameraController gc = MakeController();
  Camera cam = MakeCamera(gc);
  const glm::vec3 before = GroundUnder(cam, pixel);
  const glm::vec3 focus_before = gc.focus;

  ZoomAtCursor(gc, cam, -3.0f, pixel, kScreen);  // out
  glm::vec3 after = GroundUnder(cam, pixel);
  CHECK(after.x == Catch::Approx(before.x).margin(1e-2));
  CHECK(after.z == Catch::Approx(before.z).margin(1e-2));
  CHECK(gc.height > 60.0f);

  ZoomAtCursor(gc, cam, 3.0f, pixel, kScreen);  // back in
  after = GroundUnder(cam, pixel);
  CHECK(after.x == Catch::Approx(before.x).margin(1e-2));
  CHECK(after.z == Catch::Approx(before.z).margin(1e-2));
  // Anchored zoom out-and-back returns the camera where it started.
  CHECK(gc.height == Catch::Approx(60.0f).margin(1e-2));
  CHECK(gc.focus.x == Catch::Approx(focus_before.x).margin(1e-2));
  CHECK(gc.focus.z == Catch::Approx(focus_before.z).margin(1e-2));
}

TEST_CASE("ZoomAtCursor: zooming toward the cursor moves focus toward it") {
  // The visible consequence of anchoring: zooming in on an off-centre point
  // pulls the camera toward it.
  const glm::vec2 pixel(kW * 0.9f, kH * 0.5f);

  GameCameraController gc = MakeController();
  Camera cam = MakeCamera(gc);
  const glm::vec3 target = GroundUnder(cam, pixel);
  const float dist_before = glm::length(glm::vec2(target.x - gc.focus.x,
                                                  target.z - gc.focus.z));

  ZoomAtCursor(gc, cam, 3.0f, pixel, kScreen);

  const float dist_after = glm::length(glm::vec2(target.x - gc.focus.x,
                                                 target.z - gc.focus.z));
  INFO("focus->target: " << dist_before << " -> " << dist_after);
  CHECK(dist_after < dist_before);
}

TEST_CASE("ZoomAtCursor: a cursor above the horizon still zooms") {
  // No ground hit to anchor on -> must fall back to a plain zoom rather than
  // silently doing nothing.
  GameCameraController gc = MakeController();
  gc.pitch_deg = 5.0f;  // nearly horizontal: the top of the screen is sky
  Camera cam = MakeCamera(gc);
  const float start = gc.height;

  ZoomAtCursor(gc, cam, 2.0f, glm::vec2(kW * 0.5f, 0.0f), kScreen);
  CHECK(gc.height < start);
}

TEST_CASE("OrbitCameraController: wheel zoom is symmetric and safe") {
  // Same contract as the game camera's Zoom (both go through ZoomScalar). The
  // old linear form `distance *= (1 - wheel_y * zoom_speed)` drifted on every
  // in/out pair and collapsed to min_distance at exactly 1/zoom_speed notches
  // in a single event — reachable from a coalesced trackpad flick.
  OrbitCameraController orbit;
  orbit.distance = 8.0f;
  orbit.min_distance = 0.5f;
  orbit.max_distance = 100.0f;
  const float start = orbit.distance;

  // Positive wheel (scroll up) zooms IN -- direction preserved from the old form.
  orbit.HandleMouseWheel(1.0f);
  CHECK(orbit.distance < start);

  // Reversible.
  orbit.HandleMouseWheel(-1.0f);
  CHECK(orbit.distance == Catch::Approx(start).margin(1e-3));

  // 1/zoom_speed notches halves rather than annihilating (the old form's
  // multiply-by-zero point).
  orbit.HandleMouseWheel(1.0f / orbit.zoom_speed);
  CHECK(orbit.distance == Catch::Approx(start * 0.5f).margin(1e-3));
  orbit.HandleMouseWheel(-1.0f / orbit.zoom_speed);
  CHECK(orbit.distance == Catch::Approx(start).margin(1e-3));

  // Clamped, never <= 0, no matter the delta.
  orbit.HandleMouseWheel(1e6f);
  CHECK(orbit.distance == Catch::Approx(orbit.min_distance));
  CHECK(orbit.distance > 0.0f);
  orbit.HandleMouseWheel(-1e6f);
  CHECK(orbit.distance == Catch::Approx(orbit.max_distance));
}

TEST_CASE("ZoomAtCursor: clamped zoom does not drag focus") {
  // At the limit Zoom() is a no-op, so the anchor correction must be a no-op
  // too -- otherwise scrolling at max zoom would slowly pan the camera.
  GameCameraController gc = MakeController();
  gc.height = gc.max_height;
  Camera cam = MakeCamera(gc);
  const glm::vec3 focus_before = gc.focus;

  ZoomAtCursor(gc, cam, -5.0f, glm::vec2(kW * 0.8f, kH * 0.2f), kScreen);

  CHECK(gc.height == Catch::Approx(gc.max_height));
  CHECK(gc.focus.x == Catch::Approx(focus_before.x).margin(1e-3));
  CHECK(gc.focus.z == Catch::Approx(focus_before.z).margin(1e-3));
}
