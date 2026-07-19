#pragma once

// Camera picking: turn a pixel into a world-space ray. Engine-generic — no game
// types, no scene knowledge; callers intersect the ray with whatever they own.

#include <glm/glm.hpp>

namespace badlands {

struct Camera;

struct Ray {
  glm::vec3 origin{0.0f};
  glm::vec3 dir{0.0f, 0.0f, -1.0f};  // normalized

  glm::vec3 At(float t) const { return origin + dir * t; }
};

// World-space ray through `pixel` (window coordinates, origin top-left, +y down)
// for a window of `screen_size` pixels. The centre pixel yields exactly
// `camera.direction`.
//
// Built from the camera basis rather than by inverting the view-projection:
// Camera::GetProj applies a reversed-Z remap (near->1, far->0), which
// glm::unProject and any naive inverse-VP unprojection would get wrong. The
// basis here matches GetView()'s glm::lookAt: right = normalize(fwd x up),
// true_up = right x fwd. Ported from the legacy Rust AngledCamera::
// screen_to_ground.
//
// Horizontal extent comes from camera.aspect (what GetProj actually uses), not
// screen_size's ratio, so the ray tracks the rendered image even if the two
// disagree; screen_size only maps the pixel to NDC.
//
// NOTE: `pixel` and `screen_size` must be in the SAME coordinate space. SDL
// mouse events are in logical points while a window's drawable size is in
// physical pixels — on a HiDPI display those differ, and mixing them offsets the
// ray. Prefer the logical size (SDL_GetWindowSize) with SDL mouse coordinates.
Ray ScreenPointToRay(const Camera& camera, glm::vec2 pixel,
                     glm::vec2 screen_size);

// First intersection of `ray` with the horizontal plane y == plane_y.
//
// Returns false when the ray is parallel to the plane or points away from it
// (the hit would be behind the origin) — e.g. a cursor on the horizon or above
// it. Callers must handle that: it is the normal case near the skyline, not an
// error.
bool IntersectGroundPlane(const Ray& ray, float plane_y, glm::vec3& out_hit);

}  // namespace badlands
