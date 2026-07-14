#include "engine/app/game_camera_controller.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/camera.hpp"

namespace badlands {

void GameCameraController::Pan(glm::vec2 world_dxz) {
  focus.x += world_dxz.x;
  focus.z += world_dxz.y;
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

}  // namespace badlands
