#pragma once

// Task S2.A3: shared, game-agnostic game camera controller. Mirrors the
// legacy Rust src/game/angled_camera.rs's feel (fixed pitch + XZ pan); used by
// the game, AI sandbox and map viewer apps.

#include <glm/glm.hpp>

namespace badlands {

class Camera;

// Fixed-pitch camera that flies over the ground plane. The camera looks at
// `focus` (a point on y=0) from an offset determined by pitch_deg + height;
// panning moves `focus` in the XZ plane and zooming changes `height`. The tilt
// and compass heading never change — only where you are and how high.
class GameCameraController {
 public:
  glm::vec3 focus{0.0f};    // point on the ground plane the camera looks at
  float pitch_deg = 55.0f;  // FIXED down-tilt
  float height = 30.0f;     // offset above the plane (the zoom axis)
  float pan_speed = 20.0f;  // world units / sec for key pan

  // Zoom limits, in the same units as `height`. Apps set their own range: the
  // useful span for a 96 m skirmish is not the one for a 2 km map.
  float min_height = 5.0f;
  float max_height = 400.0f;

  // Height octaves per wheel notch: height *= 2^(-notches * zoom_speed), so
  // 1/zoom_speed notches halve the height. Multiplicative on purpose — see
  // Zoom().
  float zoom_speed = 0.25f;

  // focus.x += world_dxz.x; focus.z += world_dxz.y.
  void Pan(glm::vec2 world_dxz);

  // Zoom by `notches` (positive == scroll away == move closer to the ground),
  // clamped to [min_height, max_height].
  //
  // Multiplicative, not additive: zoom steps must be proportional to the
  // current height or one notch is imperceptible up high and slams into the
  // ground down low. It is also exactly symmetric (zoom in N then out N returns
  // the original height) and can never reach 0 or go negative, unlike the
  // `height *= (1 - notches * speed)` form.
  void Zoom(float notches);

  // Places the camera behind+above `focus` (looking toward -Z, tilted down
  // by pitch_deg) and orients it via cam.LookAt(focus).
  void UpdateCamera(Camera& cam) const;
};

// Zoom by `notches` while keeping the ground point under `pixel` pinned in
// place, then refresh `cam` — the "zoom toward the cursor" behaviour.
//
// Anchors on the y=0 ground PLANE, not on terrain: that keeps this usable by
// every app (only the map viewer has a heightmap) and matches what the fixed
// pitch already assumes. Over tall relief the anchor drifts from the visible
// surface by roughly the terrain height, which is not noticeable at these tilts.
//
// `pixel` and `screen_size` must share a coordinate space — pass SDL's mouse
// position with the window's LOGICAL size (see EventWindowLogicalSize), not the
// physical drawable size.
//
// Falls back to a plain Zoom() when the cursor has no ground hit (on/above the
// horizon), so zooming at the sky still works instead of doing nothing.
void ZoomAtCursor(GameCameraController& controller, Camera& cam, float notches,
                  glm::vec2 pixel, glm::vec2 screen_size);

}  // namespace badlands
