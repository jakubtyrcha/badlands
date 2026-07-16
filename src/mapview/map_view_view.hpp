#pragma once

// badlands_mapview's AppView: generates a map in-process (the mapgen pipeline),
// tessellates it into indexed kTerrainBlend chunks rendered with the biome
// texture-array terrain material, and views it with the fixed-angle
// GameCameraController. Terrain entities are created directly in the registry
// (no SceneGraph — the terrain is a raw indexed mesh, not a MeshAttachment).

#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/sections.hpp"

namespace badlands {

class MapViewView : public AppView {
 public:
  MapViewView(uint32_t seed, int resolution)
      : seed_(seed), resolution_(resolution) {}

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  uint32_t seed_;
  int resolution_;

  wgpu::Device device_;
  wgpu::Queue queue_;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  // Per-biome PBR texture arrays (albedo/normal/arm), layer index = Biome enum
  // value. Held here to keep the GPU textures alive for the material's lifetime.
  MaterialLibrary::TerrainArrays terrain_arrays_;

  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // Block grid kept for the camera-windowed debug grid (rebuilt per-frame).
  mapgen::Field2D<mapgen::Block> block_grid_;
  DebugLineBuffer grid_;  // block + section lines, only near the camera
  bool grid_visible_ = true;
  // Half-extent (in 10 m blocks) of the grid window around the camera focus.
  static constexpr int kGridRadiusBlocks = 15;

  int chunk_count_ = 0;
  int section_count_ = 0;
  float map_size_m_ = 0.0f;

  // Rebuilds grid_ with block-boundary lines + highlighted section boundaries,
  // limited to a kGridRadiusBlocks window around the camera focus projected on
  // the map plane (so cost is independent of map size — see #2 review fix).
  void RebuildVisibleGrid();
};

}  // namespace badlands
