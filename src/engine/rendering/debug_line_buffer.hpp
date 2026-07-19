#pragma once

// A CPU buffer of world-space debug line segments (game-agnostic). Populated
// each frame by the app (e.g. a block/section grid), pointed at by
// SceneContext::debug_lines, and drawn by SceneRenderer's debug-line pass as
// screen-aligned antialiased quads (see UploadDebugLines / thick_line.wesl).
// Ported from sampo's src/rendering/debug_line_buffer.{hpp,cpp}.

#include <vector>

#include <glm/glm.hpp>

namespace badlands {

struct DebugLine {
  glm::vec3 start;
  glm::vec3 end;
  glm::vec3 color;
  float thickness = 1.0f;  // in pixels
};

class DebugLineBuffer {
 public:
  std::vector<DebugLine> lines;

  void Clear() { lines.clear(); }
  bool empty() const { return lines.empty(); }

  void AddLine(glm::vec3 start, glm::vec3 end,
               glm::vec3 color = glm::vec3(1.0f, 1.0f, 0.0f),
               float thickness = 1.0f) {
    lines.push_back({start, end, color, thickness});
  }

  // 12 edges of an axis-aligned box.
  void AddBox(glm::vec3 min, glm::vec3 max, glm::vec3 color, float thickness = 1.0f);
};

}  // namespace badlands
