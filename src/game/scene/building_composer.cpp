#include "game/scene/building_composer.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "game/building_catalog.h"
#include "game/material_pack.h"
#include "game/scene/blockout_materials.hpp"

namespace badlands {

void AddBuildingToComposer(SceneComposer& composer, BuildingKind kind,
                           glm::vec2 center_world, float yaw_radians,
                           float ground_y) {
  const BuildingVisual bv = building_visual(kind);
  const RenderBox box = RenderBoxOf(kind, /*rotation_index=*/0);
  std::vector<BuildingPart> parts =
      BuildBuildingParts(box.size_x, box.size_z, bv.height, bv.roof);

  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0f),
                     glm::vec3(center_world.x, ground_y, center_world.y)) *
      glm::rotate(glm::mat4(1.0f), yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));

  for (BuildingPart& part : parts) {
    const bool is_wall = (part.kind == BuildingPartKind::Wall);
    const MaterialId mat_id = is_wall ? bv.wall_material : bv.roof_material;
    const glm::vec3 blockout_color = is_wall ? blockout::kWall : blockout::kRoof;
    composer.AddObject(std::move(part.mesh), blockout_color,
                       blockout::kBuildingRoughness, material_pack(mat_id).dir,
                       transform);
  }
}

}  // namespace badlands
