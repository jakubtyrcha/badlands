// Pure-CPU tests for ExpandDebugLines — the world-space thick-line quad
// expansion used by SceneRenderer's debug-line pass. Focus: a segment that
// crosses behind the camera must be near-plane clipped, so every emitted vertex
// stays in front of the camera (clip w > 0) and finite. Before the near-plane
// clip was added, the behind-camera endpoint's vertices divided by a
// non-positive w and streaked garbage across the screen.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/debug_line_expand.hpp"

using namespace badlands;

namespace {

// Camera-OFFSET view (camera at origin) looking down -Z, matching how
// SceneRenderer passes frame matrices + a separate camera_world_pos.
struct TestCam {
  glm::vec3 world_pos{0.0f, 10.0f, 0.0f};
  glm::mat4 view =
      glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f));
  glm::mat4 proj =
      glm::perspectiveFov(glm::radians(60.0f), 1600.0f, 900.0f, 0.1f, 5000.0f);
  glm::vec2 screen{1600.0f, 900.0f};
};

}  // namespace

TEST_CASE("ExpandDebugLines near-clips a segment crossing behind the camera") {
  const TestCam cam;
  const glm::mat4 vp = cam.proj * cam.view;

  DebugLineBuffer lines;
  // Off-axis segment from clearly in front of the camera (-Z) to clearly behind
  // it (+Z). Off-axis (not through the eye) so both endpoints project to
  // distinct screen points and the behind endpoint is actually emitted — an
  // on-axis line would collapse to a degenerate quad and never exercise this.
  lines.AddLine(glm::vec3(-30.0f, 10.0f, -40.0f), glm::vec3(20.0f, 12.0f, 40.0f),
                glm::vec3(1.0f, 1.0f, 1.0f), 2.0f);

  const std::vector<float> v =
      ExpandDebugLines(lines, cam.view, cam.proj, cam.screen, cam.world_pos);

  REQUIRE(v.size() == 6u * 8u);  // one segment -> 6 verts x 8 floats

  for (size_t i = 0; i < v.size(); i += 8) {
    const glm::vec3 world(v[i], v[i + 1], v[i + 2]);
    // Re-project through the same camera-offset VP the shader would use. Every
    // emitted vertex must sit in front of the camera (clip w > 0). This FAILS
    // before the near-plane clip: the behind endpoint is emitted at its true
    // world position (clip w < 0), which the GPU rasterizes as a screen streak.
    const glm::vec4 clip = vp * glm::vec4(world - cam.world_pos, 1.0f);
    CHECK(clip.w > 0.0f);
    CHECK(std::isfinite(world.x));
    CHECK(std::isfinite(world.y));
    CHECK(std::isfinite(world.z));
  }
}

TEST_CASE("ExpandDebugLines: a fully-in-front segment expands to a real quad") {
  const TestCam cam;
  const glm::mat4 vp = cam.proj * cam.view;

  DebugLineBuffer lines;
  lines.AddLine(glm::vec3(-20.0f, 10.0f, -40.0f),
                glm::vec3(20.0f, 10.0f, -40.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                3.0f);

  const std::vector<float> v =
      ExpandDebugLines(lines, cam.view, cam.proj, cam.screen, cam.world_pos);
  REQUIRE(v.size() == 6u * 8u);

  // All verts in front, and the quad has non-zero screen area (both perpendicular
  // offsets present -> the 6 verts are not all coincident).
  glm::vec3 vmin(1e9f), vmax(-1e9f);
  for (size_t i = 0; i < v.size(); i += 8) {
    const glm::vec3 world(v[i], v[i + 1], v[i + 2]);
    const glm::vec4 clip = vp * glm::vec4(world - cam.world_pos, 1.0f);
    CHECK(clip.w > 0.0f);
    vmin = glm::min(vmin, world);
    vmax = glm::max(vmax, world);
  }
  CHECK((vmax.x - vmin.x) > 1.0f);  // spans the line length
}

TEST_CASE("ExpandDebugLines: both endpoints behind -> degenerate, no garbage") {
  const TestCam cam;

  DebugLineBuffer lines;
  lines.AddLine(glm::vec3(0.0f, 10.0f, 30.0f), glm::vec3(5.0f, 10.0f, 60.0f),
                glm::vec3(0.0f, 1.0f, 0.0f), 2.0f);

  const std::vector<float> v =
      ExpandDebugLines(lines, cam.view, cam.proj, cam.screen, cam.world_pos);
  REQUIRE(v.size() == 6u * 8u);

  // Degenerate quad: 6 coincident verts at l.start, all finite (rasterizes to
  // nothing rather than blowing up).
  for (size_t i = 0; i < v.size(); i += 8) {
    CHECK(std::isfinite(v[i]));
    CHECK(std::isfinite(v[i + 1]));
    CHECK(std::isfinite(v[i + 2]));
    CHECK(v[i] == Catch::Approx(0.0f));
    CHECK(v[i + 1] == Catch::Approx(10.0f));
    CHECK(v[i + 2] == Catch::Approx(30.0f));
  }
}
