#include "engine/rendering/debug_line_buffer.hpp"

#include <cmath>

namespace badlands {

void DebugLineBuffer::AddBox(glm::vec3 min, glm::vec3 max, glm::vec3 color,
                            float thickness) {
  const glm::vec3 c[8] = {
      {min.x, min.y, min.z}, {max.x, min.y, min.z},
      {max.x, min.y, max.z}, {min.x, min.y, max.z},
      {min.x, max.y, min.z}, {max.x, max.y, min.z},
      {max.x, max.y, max.z}, {min.x, max.y, max.z},
  };
  const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) AddLine(c[e[0]], c[e[1]], color, thickness);
}

void DebugLineBuffer::AddOrientedBox(glm::vec2 center, float rotation,
                                     glm::vec2 half_extent, float y0, float y1,
                                     glm::vec3 color, float thickness) {
  const float cs = std::cos(rotation), sn = std::sin(rotation);
  // Footprint corners (local -> world XZ), same +yaw frame as the emitter.
  auto corner = [&](float lx, float lz) {
    return glm::vec2(center.x + lx * cs - lz * sn, center.y + lx * sn + lz * cs);
  };
  const glm::vec2 f[4] = {
      corner(-half_extent.x, -half_extent.y), corner(half_extent.x, -half_extent.y),
      corner(half_extent.x, half_extent.y), corner(-half_extent.x, half_extent.y)};
  glm::vec3 c[8];
  for (int i = 0; i < 4; ++i) {
    c[i] = {f[i].x, y0, f[i].y};      // bottom ring
    c[i + 4] = {f[i].x, y1, f[i].y};  // top ring
  }
  const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) AddLine(c[e[0]], c[e[1]], color, thickness);
}

}  // namespace badlands
