#pragma once

// badlands_mapview's AppView: generates a map in-process (the mapgen pipeline),
// tessellates it into indexed kTerrainBlend chunks rendered with the biome
// texture-array terrain material, and views it with the fixed-angle
// GameCameraController. Terrain entities are created directly in the registry
// (no SceneGraph — the terrain is a raw indexed mesh, not a MeshAttachment).
//
// Hovering the mouse over the terrain draws a block/section debug grid around
// the hit point (see RebuildVisibleGrid).

#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <vector>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material_library.hpp"
#include "game/geometry/terrain_clusters.hpp"
#include "mapgen/config.hpp"
#include "mapgen/pipeline.hpp"

namespace badlands {

class MapViewView : public AppView {
 public:
  // `cfg` is the full generator config (seed/size/thresholds/terracing/...), so
  // everything --config exposes reaches the viewer. `use_cluster_terrain`
  // selects the cluster-LOD terrain (default) vs the legacy fixed-subdiv chunks
  // (the A/B baseline); the ImGui checkbox flips it at runtime.
  explicit MapViewView(mapgen::MapgenConfig cfg, bool use_cluster_terrain = true)
      : cfg_(std::move(cfg)), use_cluster_terrain_(use_cluster_terrain) {}

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  mapgen::MapgenConfig cfg_;

  wgpu::Device device_;
  wgpu::Queue queue_;

  MaterialLibrary matlib_;
  CubemapBuilder sky_cube_;

  // Daylight (Hosek-Wilkie sky + directional sun), same system the game uses.
  // Static rather than a running cycle: this is a map inspector, so the light
  // holds still unless you scrub it. 0.5 == noon (daylight.cpp's solar arc).
  DaylightConfig daylight_cfg_;
  float time_of_day_ = 0.5f;
  void ApplyDaylight();  // re-bakes sky + IBL; not cheap, call on change only

  // Per-biome PBR texture arrays (albedo/normal/arm), layer index = Biome enum
  // value. Held here to keep the GPU textures alive for the material's lifetime.
  MaterialLibrary::TerrainArrays terrain_arrays_;

  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // The generated map. `heightmap` is kept for mouse picking and `graph` for
  // per-section heights — both outlive Initialize, unlike the chunk tessellation
  // inputs.
  mapgen::MapArtifacts map_;

  // Nanite-style terrain cluster-LOD DAG, built from the heightmap at load.
  // Rendered (M2) as one entity whose MeshDrawRangesComponent draws all leaf
  // clusters; LOD selection over these ranges lands in a later milestone.
  TerrainClusterDag terrain_dag_;

  // Deferred material for the cluster terrain (game-owned; no engine-side
  // MaterialLibrary entry). Held for the terrain entity's lifetime.
  std::unique_ptr<MaterialInstanceFactory> cluster_factory_;

  // Which terrain path is live. Only the selected set of entities exists in the
  // registry at a time (RebuildTerrain destroys one and builds the other).
  bool use_cluster_terrain_ = true;
  entt::entity cluster_entity_ = entt::null;  // valid iff cluster terrain live
  std::vector<entt::entity> legacy_entities_;  // valid iff legacy terrain live

  // (Re)build the live terrain path from use_cluster_terrain_, destroying the
  // other. Builds the cluster entity (all leaf clusters as draw ranges) or the
  // legacy fixed-subdiv chunk entities.
  void RebuildTerrain();
  void BuildClusterTerrain();
  void BuildLegacyTerrain();

  DebugLineBuffer grid_;  // block + section lines, only around the hover point
  bool grid_visible_ = true;
  // Half-extent (in blocks, kBlockSizeM each) of the grid window around the hover point.
  // Runtime, not compile-time: it's a debug-view knob (ImGui slider), not a
  // structural property of the map.
  int grid_radius_blocks_ = 8;

  // Where the mouse ray last hit the terrain. `hover_valid_` is false when the
  // cursor is off the terrain (sky / past the map edge) — the grid hides.
  glm::vec3 hover_point_{0.0f};
  bool hover_valid_ = false;

  int chunk_count_ = 0;
  float map_size_m_ = 0.0f;

  // Rebuilds grid_ with block-boundary lines + highlighted section boundaries,
  // limited to a grid_radius_blocks_ window around hover_point_ (so cost is
  // independent of map size). Every line sits at its block's SECTION height, so
  // each terrace reads as one flat plane rather than stair-stepping per block.
  void RebuildVisibleGrid();

  // World height of the terrace `b` belongs to (+0 lift). Sections are the
  // flat-ish regions the grid visualizes; nodes[i].id == i, so this is a direct
  // index (see mapgen::MapArtifacts::graph).
  float SectionHeight(const mapgen::Block& b) const;
};

}  // namespace badlands
