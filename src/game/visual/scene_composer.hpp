#pragma once

// A thin game-side visual entity-component layer that decouples what a game
// object *is* from how it renders. Terrain, water, and each building are
// entities in an entt registry carrying a geometry component + a material
// component; RenderMode picks a Blockout (debug) or Detailed (PBR) material per
// object at build time. ComposeInto() walks the registry and emits every entity
// into the engine's SceneGraph (which the renderer already consumes), so the
// engine render path stays untouched. The mesh is shared across proxies for now
// ("mesh equivalent"); only the material differs.

#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "game/geometry/terrain_mesh.hpp"
#include "game/visual/render_mode.hpp"

namespace badlands {

class SceneComposer {
 public:
  explicit SceneComposer(RenderMode mode = RenderMode::Detailed) : mode_(mode) {}

  RenderMode mode() const { return mode_; }

  // Terrain: a kTerrainBlend mesh. Its material (debug vs PBR arrays) comes from
  // ComposeInto's terrain_arrays argument, which the caller builds per mode.
  void AddTerrain(TerrainMesh&& mesh, const glm::mat4& world = glm::mat4(1.0f));

  // Water surface (forward-transparent). `params` is the mode-appropriate look
  // (BlockoutWaterParams / DefaultWaterParams); the factory is ComposeInto's arg.
  void AddWater(TexturedMeshResult&& mesh, const InstanceParams& params,
                const glm::mat4& world = glm::mat4(1.0f));

  // Object mesh (building part; later units). Attaches a Blockout material
  // (solid `blockout_color`) or a Detailed material (PBR `pack_dir`) per mode_.
  void AddObject(TexturedMeshResult&& mesh, glm::vec3 blockout_color,
                 float blockout_roughness, std::string pack_dir,
                 const glm::mat4& world = glm::mat4(1.0f));

  // Emit every entity into `scene`. `terrain_arrays` backs the terrain material;
  // `water_factory` backs the water surfaces -- both mode-appropriate and owned
  // by the caller (must outlive rendering).
  void ComposeInto(SceneGraph& scene, MaterialLibrary& matlib,
                   const MaterialLibrary::TerrainArrays& terrain_arrays,
                   MaterialInstanceFactory* water_factory);

 private:
  RenderMode mode_;
  entt::registry registry_;
};

}  // namespace badlands
