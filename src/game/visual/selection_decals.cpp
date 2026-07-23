#include "game/visual/selection_decals.hpp"

#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace badlands {

namespace {

// The selection-highlight look: marching ants (alternating opaque white and
// black dashes, scrolling). Fixed, not configurable -- see the header.
constexpr float kLineWidth = 0.12f;    // outline thickness, world units
constexpr float kDashLength = 0.35f;   // world units of dash...
constexpr float kDashGap = 0.35f;      // ...and of gap
constexpr float kScrollSpeed = 0.35f;  // dash periods per second

// How far outside the entity's own footprint the outline sits, so the highlight
// reads as a ring AROUND the thing rather than clipping into it.
constexpr float kUnitMargin = 0.25f;
constexpr float kBuildingMargin = 0.6f;

constexpr float kBuildingCornerRadius = 0.45f;

// Vertical band the decal projects through: generous enough to cover terrain
// undulation under a footprint, tight enough to stay off rooftops.
constexpr float kProjectorHalfHeight = 1.5f;

// Applies the shared line style to a decal, leaving only the shape/placement to
// the caller.
void ApplyStyle(ProjectedDecal& decal) {
  decal.line_width = kLineWidth;
  decal.dash_length = kDashLength;
  decal.dash_gap = kDashGap;
  decal.scroll_speed = kScrollSpeed;
  decal.projector_half_height = kProjectorHalfHeight;
  decal.color_a = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // white
  decal.color_b = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // black
}

}  // namespace

float YawFromRotationIndex(int32_t rotation_index) {
  return glm::radians(static_cast<float>(rotation_index) * 45.0f);
}

ProjectedDecal MakeUnitRing(const CharacterState& unit, float ground) {
  ProjectedDecal decal;
  decal.shape = DecalShape::Ring;
  decal.center = glm::vec3(unit.pos_x, ground, unit.pos_z);
  decal.yaw = 0.0f;  // a circle has no orientation
  // The unit's capsule is drawn with radius 0.5*min(size_x, size_z); size the
  // ring off the LARGER extent so the highlight encloses the whole footprint.
  const float radius = 0.5f * std::max(unit.size_x, unit.size_z) + kUnitMargin;
  decal.half_extents = glm::vec2(radius);
  ApplyStyle(decal);
  return decal;
}

ProjectedDecal MakeBuildingRect(const BuildingState& building, float ground) {
  ProjectedDecal decal;
  decal.shape = DecalShape::RoundedRect;
  decal.center = glm::vec3(building.center_x, ground, building.center_z);
  decal.yaw = YawFromRotationIndex(building.rotation_index);

  // The unrotated render box + the placement yaw == exactly how the building
  // mesh is transformed, so the outline hugs the drawn walls.
  const RenderBox box = RenderBoxOf(building.kind, 0);
  decal.half_extents = glm::vec2(0.5f * box.size_x + kBuildingMargin,
                                 0.5f * box.size_z + kBuildingMargin);
  // A corner radius larger than the half-size would be clamped by the shader
  // anyway; clamp here too so the value the decal carries is the one used.
  decal.corner_radius =
      std::min(kBuildingCornerRadius,
               std::min(decal.half_extents.x, decal.half_extents.y));
  ApplyStyle(decal);
  return decal;
}

}  // namespace badlands
