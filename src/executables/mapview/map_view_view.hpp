#pragma once

// badlands_mapview's AppView: generates a map in-process via
// mapgen::generate_map, wraps it in the frozen MapData contract, and renders
// it as Nanite-style cluster-LOD terrain (the shared ClusterTerrain module)
// with the fixed-angle GameCameraController. Terrain is one entity holding the
// shared cluster mesh; a MeshDrawRangesComponent carries the per-frame LOD
// cut. Entities are created directly in the registry (no SceneGraph -- the
// terrain is a raw indexed mesh, not a MeshAttachment).
//
// Beyond the terrain the view carries a shared SimClock driving the sun and
// cursor-anchored zoom. Hovering the mouse over the terrain shows its world
// position + dominant biome. (The old fog-emitter system was removed pending
// a rewrite.)

#include <cstdint>

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
#include "game/map/cluster_terrain.hpp"
#include "game/map/map_data.hpp"
#include "mapgen/generator.hpp"

namespace badlands {

class SceneRenderer;

class MapViewView : public AppView {
 public:
  // `params` is the generator params (seed/resolution/size), so everything the
  // CLI exposes reaches the viewer. `camera_height` overrides the starting
  // camera height (0 = keep the default ground-level framing); `lod_tint`
  // seeds the cluster debug tint (0 shaded / 1 triangle hash / 2 LOD level).
  // `serial_build` forces the single-threaded DAG build (the perf A/B
  // baseline; default is the parallel build). The overrides exist mainly so
  // headless --screenshot runs can frame near/far and set the tint without
  // touching the interactive defaults.
  explicit MapViewView(mapgen::MapGenParams params, float camera_height = 0.0f,
                       int lod_tint = 0, bool serial_build = false)
      : params_(params),
        camera_height_override_(camera_height),
        initial_tint_(lod_tint),
        serial_build_(serial_build) {}

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  mapgen::MapGenParams params_;

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;  // shared, owned by the app
  float dt_ = 0.0f;                          // last real frame dt (for the FPS line)

  CubemapBuilder sky_cube_;

  // Daylight (Hosek-Wilkie sky + directional sun), same system the game uses,
  // driven by the shared SimClock (play/pause/speed + scrub). Seeded to noon;
  // starts paused (an inspector).
  DaylightConfig daylight_cfg_;
  SimClock sim_clock_;
  void ApplyDaylight();  // re-bakes sky + IBL; not cheap, call on change only

  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // The generated map. `heightmap` is kept for mouse picking, `bedrock` for
  // previews/erosion later -- both outlive Initialize.
  mapgen::MapArtifacts map_;
  // The generator output wrapped in the frozen MapData contract (one-hot biome
  // slices at the raster's own texel spacing) -- what the cluster terrain
  // builder and mouse picking read.
  MapData terrain_map_;

  // The shared cluster-LOD terrain module: owns the DAG, its vertex-color
  // material factory, the terrain entity, the per-frame LOD cut, and the Terrain
  // debug UI. Built with an identity model (mapview vertices are absolute world).
  ClusterTerrain cluster_terrain_;

  // Where the mouse ray last hit the terrain. `hover_valid_` is false when the
  // cursor is off the terrain (sky / past the map edge) -- the hover UI hides.
  glm::vec3 hover_point_{0.0f};
  bool hover_valid_ = false;

  float map_size_m_ = 0.0f;

  // Starting camera height override (0 = default); applied once in Initialize.
  float camera_height_override_ = 0.0f;
  // Debug tint seed for the cluster terrain (headless --lod-tint); pushed into
  // cluster_terrain_ before Build so frame one renders tinted.
  int initial_tint_ = 0;
  // Force the single-threaded cluster DAG build (perf A/B baseline); seeded once
  // in Initialize, not runtime-toggleable (the DAG is built there).
  bool serial_build_ = false;
  // Viewport height in pixels, tracked by OnResize -- the LOD screen-space-error
  // metric's numerator. Seeded so the first Update (before any resize) still has
  // a sane value in headless paths.
  float screen_h_px_ = 1080.0f;
};

}  // namespace badlands
