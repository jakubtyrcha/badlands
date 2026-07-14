#include "game/scene/building_scene.h"

#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "engine/rendering/scene_build.hpp"
#include "game/building_catalog.h"
#include "game/material_pack.h"

namespace badlands {

Aabb AddBuildingToScene(SceneGraph& scene, MaterialLibrary& matlib,
                        GameBuildingKind kind, glm::vec2 center_world,
                        float yaw_radians) {
  const BuildingVisual bv = building_visual(kind);
  const GameRenderBox box =
      game_render_box(static_cast<int32_t>(kind), /*rotation_index=*/0);
  std::vector<BuildingPart> parts =
      BuildBuildingParts(box.size_x, box.size_z, bv.height, bv.roof);

  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0f),
                     glm::vec3(center_world.x, 0.0f, center_world.y)) *
      glm::rotate(glm::mat4(1.0f), yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));

  Aabb local_bounds = Aabb::Empty();
  int part_index = 0;
  for (BuildingPart& part : parts) {
    const MaterialId mat_id = (part.kind == BuildingPartKind::Wall)
                                  ? bv.wall_material
                                  : bv.roof_material;
    const MaterialPack pack = material_pack(mat_id);
    const DeferredMaterial mat = matlib.Get(pack.dir);

    local_bounds = local_bounds.Union(part.mesh.local_bounds);
    const std::string name = "building_part_" + std::to_string(part_index++);
    AddMeshEntity(scene, name.c_str(), std::move(part.mesh), mat, transform);
  }
  return local_bounds;
}

}  // namespace badlands
