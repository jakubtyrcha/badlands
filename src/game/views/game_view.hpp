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
#include "engine/app/sim_clock.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class GameView : public AppView {
 public:
  ~GameView() override;

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  // Jump the day/night clock to an absolute time-of-day (headless capture).
  void SeekToTimeOfDay(float t01) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  // Computes the sun/moon directional light for the clock's current time-of-day
  // and sets it into scene_ (cheap, every frame -> light + shadows move). The
  // expensive HW sky cube + SH + IBL re-bake is throttled by real time (or
  // `force_rebake_`). Mirrors the result into scene_ (its per-frame
  // SyncToRegistry would otherwise clobber it with SceneGraph's own defaults).
  void UpdateDaylight();
  // Immediately recomputes the light AND re-bakes the sky for the current
  // time-of-day, bypassing the throttle. Used at init and on a time-of-day seek
  // so a single frame is correct.
  void ApplyDaylightNow();
  // Bakes the HW sky/IBL/ambient/directional-light for `state` into
  // scene_context_ and mirrors ambient into scene_. The expensive path.
  void RebakeSky(const DaylightState& state);
  // Seeds the demo town via game_dispatch(GAME_ACTION_PLACE_BUILDING) at a
  // few spread-out, non-overlapping tiles around the prebuilt origin Castle.
  void PlaceDemoBuildings();
  // Clears scene_ and rebuilds it from scratch: re-mirrors scene_context_'s
  // lighting, adds the gray floor, then adds every game_buildings() row via
  // AddBuildingToScene. Called once from Initialize. NOTE: the sim now ticks
  // (Update runs game_tick at a fixed rate), but this stage has no dynamic
  // entities, so the building set never changes and the scene stays valid.
  // When dynamic entities land, BuildScene (or an incremental scene update)
  // must be re-driven from the sim each frame the world changes.
  void BuildScene();

  // GPU handles (from RenderContext, stored so DrawUI can re-bake the sky when
  // a DaylightConfig value changes live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  CubemapBuilder sky_cube_;

  // Time model (see sim_clock.hpp). `sim_clock_` accumulates real dt * speed;
  // the day/night cycle reads TimeOfDay()/DayCounter() and the fixed-rate game
  // logic runs up to TickTarget() (`sim_ticks_done_` tracks how many game_ticks
  // have run). The expensive sky re-bake is throttled by REAL time
  // (`rebake_accum_`), so an Update(0) never re-bakes; `force_rebake_` requests
  // an immediate re-bake (a DaylightConfig edit in DrawUI).
  DaylightConfig daylight_cfg_;
  SimClock sim_clock_;
  unsigned long long sim_ticks_done_ = 0;
  double rebake_accum_ = 0.0;
  bool force_rebake_ = false;

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
