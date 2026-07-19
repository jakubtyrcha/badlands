#pragma once

#include <array>

#include <glm/glm.hpp>

#include "engine/rendering/geometry/aabb.hpp"

namespace badlands {

// View frustum as 6 inward-pointing planes (Gribb–Hartmann), for AABB culling.
// Build from a world view-projection matrix; test world-space AABBs. The planes
// bound the frustum regardless of the reversed-Z depth convention (reversed-Z
// only remaps the depth value, not the geometric frustum).
struct Frustum {
  // Each plane is (a, b, c, d): a point p is inside/on when a*p.x + b*p.y +
  // c*p.z + d >= 0. Order: left, right, bottom, top, near, far.
  std::array<glm::vec4, 6> planes;

  static Frustum FromViewProj(const glm::mat4& view_proj);

  // Conservative test: false only when `box` is fully outside the frustum.
  bool Intersects(const Aabb& box) const;
};

}  // namespace badlands
