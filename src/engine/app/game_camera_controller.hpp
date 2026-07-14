#pragma once

// Task S2.A3: shared, game-agnostic game camera controller. Mirrors the
// legacy Rust src/game/angled_camera.rs's feel (fixed pitch + fixed height +
// XZ pan); used by the game + AI sandbox apps.

#include <glm/glm.hpp>

namespace badlands {

class Camera;

// Fixed-pitch, fixed-height camera that flies over the ground plane. The
// camera looks at `focus` (a point on y=0) from a FIXED offset determined by
// pitch_deg + height; panning moves `focus` in the XZ plane. No
// rotation/zoom.
class GameCameraController {
 public:
  glm::vec3 focus{0.0f};    // point on the ground plane the camera looks at
  float pitch_deg = 55.0f;  // FIXED down-tilt
  float height = 30.0f;     // FIXED offset above the plane
  float pan_speed = 20.0f;  // world units / sec for key pan

  // focus.x += world_dxz.x; focus.z += world_dxz.y.
  void Pan(glm::vec2 world_dxz);

  // Places the camera behind+above `focus` (looking toward -Z, tilted down
  // by pitch_deg) and orients it via cam.LookAt(focus).
  void UpdateCamera(Camera& cam) const;
};

}  // namespace badlands
