#pragma once

// Task S2.F: badlands_game's real AppView -- renders the sim's buildings
// (static this stage: no game_tick, no unit spawns) through the shared
// deferred renderer + LightEnvironment, viewed with the shared fixed-angle
// GameCameraController (WASD/arrow XZ pan; F is its first consumer).
// Replaces PlaceholderView for the game executable. Terrain/ploppables/units
// are NOT in this stage -- buildings only.

#include <cstdint>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "badlands_game.h"  // BadlandsGame, GameBuildingKind
#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class GameView : public AppView {
 public:
  ~GameView() override;

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  // Re-derives env_'s sky cube / SH ambient / sun into scene_context_, then
  // mirrors the result into scene_ (its per-frame SyncToRegistry would
  // otherwise clobber it with SceneGraph's own defaults -- same pattern as
  // ModelViewerView/PlaceholderView).
  void ApplyEnvironment();
  // Seeds the demo town via game_dispatch(GAME_ACTION_PLACE_BUILDING) at a
  // few spread-out, non-overlapping tiles around the prebuilt origin Castle.
  void PlaceDemoBuildings();
  // Clears scene_ and rebuilds it from scratch: re-mirrors scene_context_'s
  // lighting, adds the gray floor, then adds every game_buildings() row via
  // AddBuildingToScene. Called once from Initialize -- this stage's
  // buildings are static (no game_tick, no further placement UI), so nothing
  // else triggers a rebuild yet.
  void BuildScene();

  // GPU handles (from RenderContext, stored so DrawUI can re-run
  // ApplyEnvironment when the light-environment editor changes env_ live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // Owns the sim; created in Initialize, destroyed in ~GameView.
  BadlandsGame* game_ = nullptr;

  // Reused read-back buffer for game_buildings() (BuildScene + DrawUI), sized
  // to kMaxBuildingRows once -- avoids a per-frame heap allocation.
  std::vector<GameBuildingState> building_rows_;

  float dt_ = 0.0f;
};

}  // namespace badlands
