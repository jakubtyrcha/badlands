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

uint32_t BuildingAtWorld(const BuildingState* buildings, uint32_t count,
                         glm::vec2 world) {
  if (!buildings) return kNoPick;
  // Back to front: the last-placed building wins an overlap.
  for (uint32_t i = count; i-- > 0;) {
    const BuildingState& b = buildings[i];
    const RenderBox box = RenderBoxOf(b.kind, b.rotation_index);
    const glm::vec2 center(b.center_x, b.center_z);
    const glm::vec2 half(box.size_x * 0.5f, box.size_z * 0.5f);
    if (PointInOrientedBox(center, half, box.yaw_radians, world)) return b.id;
  }
  return kNoPick;
}

uint32_t HeroAtWorld(const CharacterState* characters, uint32_t count,
                     glm::vec2 world) {
  if (!characters) return kNoPick;
  for (uint32_t i = count; i-- > 0;) {
    const CharacterState& ch = characters[i];
    if (ch.inside_building_id >= 0) continue;  // hidden, so not clickable
    const glm::vec2 center(ch.pos_x, ch.pos_z);
    const glm::vec2 half(ch.size_x * 0.5f, ch.size_z * 0.5f);
    // Characters are drawn axis-aligned, so no yaw here.
    if (PointInOrientedBox(center, half, 0.0f, world)) return ch.id;
  }
  return kNoPick;
}

bool GroundPickXZ(glm::vec3 ray_origin, glm::vec3 ray_dir,
                  const std::function<float(glm::vec2)>& height_at,
                  glm::vec2& out_xz, int iterations) {
  if (ray_dir.y >= 0.0f) return false;  // not descending toward the ground
  float plane_y = 0.0f;
  glm::vec2 xz(0.0f);
  for (int i = 0; i < iterations; ++i) {
    const float t = (plane_y - ray_origin.y) / ray_dir.y;
    if (t <= 0.0f) return false;  // the ground plane is behind the camera
    xz = glm::vec2(ray_origin.x + t * ray_dir.x, ray_origin.z + t * ray_dir.z);
    plane_y = height_at(xz);  // refine: next intersect uses the sampled height
  }
  out_xz = xz;
  return true;
}

const CharacterState* SelectedUnit(const CharacterState* characters,
                                   uint32_t count, uint32_t id) {
  if (!characters || id == kNoPick) return nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const CharacterState& ch = characters[i];
    if (ch.id != id) continue;
    // Found it, but a unit that walked into a building is no longer drawn or
    // pickable -- treat it as gone so the caller clears the selection.
    return ch.inside_building_id >= 0 ? nullptr : &characters[i];
  }
  return nullptr;
}

}  // namespace badlands
