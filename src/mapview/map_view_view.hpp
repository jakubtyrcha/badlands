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
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/app/sim_clock.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/fog_sim.hpp"
#include "engine/rendering/material_library.hpp"
#include "mapgen/config.hpp"
#include "mapgen/fog_generator.hpp"  // BorderFogParams
#include "mapgen/pipeline.hpp"

namespace badlands {

class SceneRenderer;

class MapViewView : public AppView {
 public:
  // `cfg` is the full generator config (seed/size/thresholds/terracing/...), so
  // everything --config exposes reaches the viewer.
  explicit MapViewView(mapgen::MapgenConfig cfg) : cfg_(std::move(cfg)) {}

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
  SceneRenderer* scene_renderer_ = nullptr;  // shared, owned by the app
  float dt_ = 0.0f;                          // last real frame dt (for the FPS line)

  MaterialLibrary matlib_;
  CubemapBuilder sky_cube_;

  // Daylight (Hosek-Wilkie sky + directional sun), same system the game uses,
  // driven by the shared SimClock (play/pause/speed + scrub). The clock also
  // advances the fog animation. Seeded to noon; starts paused (an inspector).
  DaylightConfig daylight_cfg_;
  SimClock sim_clock_;
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

  // Biome-derived fog emitters (see mapgen::GenerateBiomeFog). Retained so they
  // can be picked/edited; pushed to the fog sim via SetFogSources.
  std::vector<fog::Emitter> fog_emitters_;
  int selected_emitter_ = -1;  // index into fog_emitters_, or -1 (none)

  // Border fog: a WORLD-STATIC milk-white fog wall around the map perimeter
  // (mapgen::BuildBorderFog from the map bounds -- fixed in world space, does NOT
  // move with the camera). Max-combined with the biome fog.
  bool border_fog_enabled_ = true;
  mapgen::BorderFogParams border_fog_;

  // Uploads fog_emitters_ + the map-border wall to the renderer's fog sim.
  void SetFogSources();
  // Emitter whose footprint contains world XZ (nearest centre), or -1.
  int PickEmitter(const glm::vec3& world) const;
  void DrawFogEmitterEditor();  // the "Fog Emitters" ImGui window

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
