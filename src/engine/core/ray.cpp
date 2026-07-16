#include "engine/core/ray.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/camera.hpp"

namespace badlands {

Ray ScreenPointToRay(const Camera& camera, glm::vec2 pixel,
                     glm::vec2 screen_size) {
  const glm::vec3 fwd = glm::normalize(camera.direction);
  const glm::vec3 right = glm::normalize(glm::cross(fwd, camera.up));
  const glm::vec3 up = glm::cross(right, fwd);

  const float w = std::max(screen_size.x, 1.0f);
  const float h = std::max(screen_size.y, 1.0f);
  const float half_h = std::tan(0.5f * glm::radians(camera.fov));  // fov is vertical
  const float half_w = half_h * camera.aspect;

  const float ndc_x = (pixel.x / w) * 2.0f - 1.0f;
  const float ndc_y = 1.0f - (pixel.y / h) * 2.0f;  // window y grows downward

  Ray ray;
  ray.origin = camera.position;
  ray.dir =
      glm::normalize(fwd + right * (ndc_x * half_w) + up * (ndc_y * half_h));
  return ray;
}

}  // namespace badlands
