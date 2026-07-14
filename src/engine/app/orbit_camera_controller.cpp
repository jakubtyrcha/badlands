#include "engine/app/orbit_camera_controller.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/camera.hpp"

namespace badlands {

namespace {
// Keep pitch strictly inside (-pi/2, pi/2) so the orbit direction never
// degenerates at the poles (cos(pitch) -> 0 would collapse yaw's effect).
constexpr float kPitchClamp = 1.5f;
// Bounding-sphere framing factor for FrameBounds: how many radii back the
// camera sits so the whole sphere comfortably fits in view.
constexpr float kFrameDistanceFactor = 3.0f;
}  // namespace

void OrbitCameraController::HandleMouseDrag(float dx_px, float dy_px) {
  yaw += dx_px * orbit_speed;
  pitch -= dy_px * orbit_speed;
  pitch = std::clamp(pitch, -kPitchClamp, kPitchClamp);
}

void OrbitCameraController::HandleMouseWheel(float wheel_y) {
  distance *= (1.0f - wheel_y * zoom_speed);
  distance = std::clamp(distance, min_distance, max_distance);
}

void OrbitCameraController::FrameBounds(const glm::vec3& center, float radius) {
  target = center;
  distance = std::clamp(radius * kFrameDistanceFactor, min_distance, max_distance);
}

void OrbitCameraController::UpdateCamera(Camera& cam) const {
  glm::vec3 dir{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
               std::cos(pitch) * std::cos(yaw)};
  cam.position = target + distance * dir;
  cam.LookAt(target);
}

}  // namespace badlands
