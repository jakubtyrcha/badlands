// Pure-CPU tests for ScreenPointToRay — the screen pixel -> world ray used for
// mouse picking.
//
// The camera basis is the easy thing to get subtly wrong (a flipped screen-Y or
// a dropped aspect term still "works" at the screen centre and only skews off-
// axis), so these pin the centre AND both off-axis directions, then check the
// whole chain end-to-end against GameCameraController: the centre pixel of a
// fixed-angle camera must land on its focus point.

#include <catch_amalgamated.hpp>

#include <cmath>

#include <glm/glm.hpp>

#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/core/ray.hpp"

using namespace badlands;

namespace {

constexpr float kW = 1600.0f;
constexpr float kH = 900.0f;

Camera TopDownIsh() {
  Camera c;
  c.position = glm::vec3(10.0f, 50.0f, 20.0f);
  c.direction = glm::normalize(glm::vec3(0.0f, -1.0f, -0.35f));  // tilted, not straight down
  c.up = glm::vec3(0.0f, 1.0f, 0.0f);
  c.fov = 45.0f;
  c.aspect = kW / kH;
  return c;
}

// Intersect a ray with the y = 0 ground plane.
bool GroundHit(const Ray& r, glm::vec3& out) {
  if (r.dir.y > -1e-4f) return false;  // not pointing down
  const float t = -r.origin.y / r.dir.y;
  out = r.At(t);
  return t > 0.0f;
}

}  // namespace

TEST_CASE("ScreenPointToRay: centre pixel looks exactly along the camera") {
  const Camera cam = TopDownIsh();
  const Ray r = ScreenPointToRay(cam, glm::vec2(kW, kH) * 0.5f,
                                 glm::vec2(kW, kH));

  CHECK(r.origin.x == Catch::Approx(cam.position.x));
  CHECK(r.origin.y == Catch::Approx(cam.position.y));
  CHECK(r.origin.z == Catch::Approx(cam.position.z));
  // Centre pixel -> no right/up offset at all.
  const glm::vec3 fwd = glm::normalize(cam.direction);
  CHECK(r.dir.x == Catch::Approx(fwd.x).margin(1e-5));
  CHECK(r.dir.y == Catch::Approx(fwd.y).margin(1e-5));
  CHECK(r.dir.z == Catch::Approx(fwd.z).margin(1e-5));
  CHECK(glm::length(r.dir) == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("ScreenPointToRay: off-centre pixels tilt the correct way") {
  const Camera cam = TopDownIsh();
  const glm::vec3 fwd = glm::normalize(cam.direction);
  const glm::vec3 right = glm::normalize(glm::cross(fwd, cam.up));
  const glm::vec3 up = glm::cross(right, fwd);

  // Right of centre -> +right component. Left -> -right.
  const Ray r_right =
      ScreenPointToRay(cam, glm::vec2(kW * 0.9f, kH * 0.5f), glm::vec2(kW, kH));
  const Ray r_left =
      ScreenPointToRay(cam, glm::vec2(kW * 0.1f, kH * 0.5f), glm::vec2(kW, kH));
  CHECK(glm::dot(r_right.dir, right) > 0.0f);
  CHECK(glm::dot(r_left.dir, right) < 0.0f);

  // Window y grows DOWNWARD: the top of the screen must tilt toward camera-up.
  // Getting this backwards inverts the hover vertically.
  const Ray r_top =
      ScreenPointToRay(cam, glm::vec2(kW * 0.5f, kH * 0.1f), glm::vec2(kW, kH));
  const Ray r_bottom =
      ScreenPointToRay(cam, glm::vec2(kW * 0.5f, kH * 0.9f), glm::vec2(kW, kH));
  CHECK(glm::dot(r_top.dir, up) > 0.0f);
  CHECK(glm::dot(r_bottom.dir, up) < 0.0f);
}

TEST_CASE("ScreenPointToRay: aspect widens X, not Y") {
  // A wide camera must spread the ray further horizontally for the same NDC.
  // Dropping the aspect term is invisible at 1:1 and only shows up here.
  Camera narrow = TopDownIsh();
  narrow.aspect = 1.0f;
  Camera wide = TopDownIsh();
  wide.aspect = 2.0f;

  const glm::vec3 fwd = glm::normalize(narrow.direction);
  const glm::vec3 right = glm::normalize(glm::cross(fwd, narrow.up));
  const glm::vec3 up = glm::cross(right, fwd);
  const glm::vec2 edge(kW, kH * 0.5f);  // right edge, vertically centred

  const Ray rn = ScreenPointToRay(narrow, edge, glm::vec2(kW, kH));
  const Ray rw = ScreenPointToRay(wide, edge, glm::vec2(kW, kH));
  CHECK(glm::dot(rw.dir, right) > glm::dot(rn.dir, right));
  // Vertical spread is unaffected by aspect.
  CHECK(glm::dot(rw.dir, up) == Catch::Approx(glm::dot(rn.dir, up)).margin(1e-5));
}

TEST_CASE("ScreenPointToRay: centre pixel of a fixed-angle camera hits focus") {
  // End-to-end against the real controller: this is the property mouse picking
  // depends on, and it fails if the basis, the screen-Y sign, or the fov
  // handling is off. (Ports the legacy AngledCamera::screen_to_ground test.)
  GameCameraController gc;
  gc.focus = glm::vec3(3.0f, 0.0f, -2.0f);
  gc.pitch_deg = 55.0f;
  gc.height = 30.0f;

  Camera cam;
  cam.fov = 45.0f;
  cam.aspect = kW / kH;
  gc.UpdateCamera(cam);

  const Ray r = ScreenPointToRay(cam, glm::vec2(kW, kH) * 0.5f,
                                 glm::vec2(kW, kH));
  glm::vec3 hit;
  REQUIRE(GroundHit(r, hit));
  CHECK(hit.x == Catch::Approx(gc.focus.x).margin(1e-3));
  CHECK(hit.z == Catch::Approx(gc.focus.z).margin(1e-3));
}
