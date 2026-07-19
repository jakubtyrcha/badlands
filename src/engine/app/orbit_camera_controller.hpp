#pragma once

// Task S2.A3: shared, game-agnostic orbit camera controller. A simple
// classic orbit (target + distance + yaw + pitch) -- NOT sampo's
// planet-surface orbit. Used by the model viewer (badlands_viewer).

#include <glm/glm.hpp>

namespace badlands {

class Camera;

// Classic orbit: camera sits at `distance` from `target`, on a sphere
// parameterized by yaw (around +Y) and pitch (clamped away from the poles).
// Drag orbits, wheel zooms. Frame with FrameBounds() for the model viewer.
class OrbitCameraController {
 public:
  glm::vec3 target{0.0f};
  float distance = 4.0f;
  float yaw = 0.6f;    // radians
  float pitch = 0.4f;  // radians, clamped to (~-1.5, ~1.5)
  float min_distance = 0.5f, max_distance = 100.0f;
  float orbit_speed = 0.01f;  // radians per pixel
  float zoom_speed = 0.1f;    // distance octaves per wheel notch

  // Drag delta in pixels: yaw += dx*orbit_speed; pitch -= dy*orbit_speed
  // (screen y grows downward, so dragging up tilts the view up); pitch is
  // clamped away from the poles to avoid gimbal flip.
  void HandleMouseDrag(float dx_px, float dy_px);

  // Wheel notches (positive = scroll up / zoom in): distance shrinks
  // multiplicatively, clamped to [min_distance, max_distance]. Callers must
  // pass a FLIPPED-normalized delta (see NormalizedWheelY) or macOS natural
  // scrolling inverts the zoom. See ZoomScalar for why this is not linear.
  void HandleMouseWheel(float wheel_y);

  // Frames `center`/`radius` (e.g. a mesh's bounding sphere): target=center,
  // distance set proportional to radius so the whole bounds fits in view.
  void FrameBounds(const glm::vec3& center, float radius);

  // Writes cam.position (target + distance*dir(yaw,pitch)) and orients it via
  // cam.LookAt(target).
  void UpdateCamera(Camera& cam) const;
};

}  // namespace badlands
