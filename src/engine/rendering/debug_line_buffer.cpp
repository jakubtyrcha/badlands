#include "engine/rendering/debug_line_buffer.hpp"

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

}  // namespace badlands
