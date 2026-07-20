#include "game/ui/picking.hpp"

#include <cmath>

namespace badlands {

bool PointInOrientedBox(glm::vec2 center, glm::vec2 half, float yaw,
                        glm::vec2 p) {
  const glm::vec2 d = p - center;
  const float s = std::sin(yaw);
  const float c = std::cos(yaw);
  // Inverse of the renderer's R_y, expressed in (x, z) -- see the sign warning
  // in the header.
  const glm::vec2 local(c * d.x - s * d.y, s * d.x + c * d.y);
  return std::fabs(local.x) <= half.x && std::fabs(local.y) <= half.y;
}

uint32_t BuildingAtWorld(const GameBuildingState* buildings, uint32_t count,
                         glm::vec2 world) {
  if (!buildings) return kNoPick;
  // Back to front: the last-placed building wins an overlap.
  for (uint32_t i = count; i-- > 0;) {
    const GameBuildingState& b = buildings[i];
    const GameRenderBox box = game_render_box(b.kind, b.rotation_index);
    const glm::vec2 center(b.center_x, b.center_z);
    const glm::vec2 half(box.size_x * 0.5f, box.size_z * 0.5f);
    if (PointInOrientedBox(center, half, box.yaw_radians, world)) return b.id;
  }
  return kNoPick;
}

uint32_t HeroAtWorld(const GameCharacterState* characters, uint32_t count,
                     glm::vec2 world) {
  if (!characters) return kNoPick;
  for (uint32_t i = count; i-- > 0;) {
    const GameCharacterState& ch = characters[i];
    if (ch.inside_building_id >= 0) continue;  // hidden, so not clickable
    const glm::vec2 center(ch.pos_x, ch.pos_z);
    const glm::vec2 half(ch.size_x * 0.5f, ch.size_z * 0.5f);
    // Characters are drawn axis-aligned, so no yaw here.
    if (PointInOrientedBox(center, half, 0.0f, world)) return ch.id;
  }
  return kNoPick;
}

}  // namespace badlands
