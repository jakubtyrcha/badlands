#include "engine/app/game_camera_controller.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/camera.hpp"
#include "engine/core/ray.hpp"

namespace badlands {

void GameCameraController::Pan(glm::vec2 world_dxz) {
  focus.x += world_dxz.x;
  focus.z += world_dxz.y;
}

void GameCameraController::Zoom(float notches) {
  height = std::clamp(height * std::exp2(-notches * zoom_speed), min_height,
                      max_height);
}

void GameCameraController::UpdateCamera(Camera& cam) const {
  const float p = glm::radians(pitch_deg);
  // Fixed compass direction (looking toward -Z, i.e. "north"), tilted down
  // by pitch_deg: back points up+south so eye = focus + back*dist sits
  // above and behind (south of) the focus point.
  const glm::vec3 back{0.0f, std::sin(p), std::cos(p)};
  const float dist = height / std::max(std::sin(p), 1e-3f);
  cam.position = focus + back * dist;
  cam.LookAt(focus);
}

void ZoomAtCursor(GameCameraController& controller, Camera& cam, float notches,
                  glm::vec2 pixel, glm::vec2 screen_size) {
  // Where the cursor points at the ground BEFORE zooming.
  glm::vec3 before;
  const bool anchored = IntersectGroundPlane(
      ScreenPointToRay(cam, pixel, screen_size), 0.0f, before);

  controller.Zoom(notches);
  controller.UpdateCamera(cam);

  if (!anchored) return;  // cursor on/above the horizon: plain zoom

  // The same pixel now points somewhere else; slide focus by the difference so
  // the original ground point lands back under the cursor. One shot rather than
  // iterative: at a fixed tilt and height, translating focus translates the
  // ground hit one-for-one, so the correction is exact.
  glm::vec3 after;
  if (!IntersectGroundPlane(ScreenPointToRay(cam, pixel, screen_size), 0.0f,
                            after)) {
    return;
  }
  controller.focus.x += before.x - after.x;
  controller.focus.z += before.z - after.z;
  controller.UpdateCamera(cam);
}

}  // namespace badlands
