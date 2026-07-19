// Pure-CPU tests for view-frustum AABB culling.

#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/frustum.hpp"

using namespace badlands;

namespace {
Aabb Box(glm::vec3 center, float r) {
  return Aabb::FromMinMax(center - glm::vec3(r), center + glm::vec3(r));
}
}  // namespace

TEST_CASE("Frustum::Intersects: inside / outside / straddling") {
  // Camera at origin looking down -Z (fov 90, aspect 1, near 0.5, far 100).
  // GLM_FORCE_DEPTH_ZERO_TO_ONE (set on this target) matches the engine's
  // [0,1] clip-z that Frustum::FromViewProj assumes.
  const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.5f, 100.0f);
  const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
  const Frustum f = Frustum::FromViewProj(proj * view);

  CHECK(f.Intersects(Box({0, 0, -10}, 1.0f)));         // straight ahead
  CHECK(f.Intersects(Box({0, 0, -1}, 5.0f)));          // straddles the near plane
  CHECK_FALSE(f.Intersects(Box({-100, 0, -10}, 1.0f)));  // far to the left
  CHECK_FALSE(f.Intersects(Box({100, 0, -10}, 1.0f)));   // far to the right
  CHECK_FALSE(f.Intersects(Box({0, 0, 10}, 1.0f)));      // behind the camera
  CHECK_FALSE(f.Intersects(Box({0, 0, -1000}, 1.0f)));   // beyond the far plane
}
