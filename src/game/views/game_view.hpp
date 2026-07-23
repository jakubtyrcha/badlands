#pragma once

// Task S2.F: badlands_game's real AppView -- renders the sim's buildings
// (static this stage: no game_tick, no unit spawns) through the shared
// deferred renderer + LightEnvironment, viewed with the shared fixed-angle
// GameCameraController (WASD/arrow XZ pan; F is its first consumer).
// Replaces PlaceholderView for the game executable. Terrain/ploppables/units
// are NOT in this stage -- buildings only.

#include <cstdint>
#include <memory>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "badlands_sim.hpp"  // badlands::Sim, badlands::BuildingState
#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/app/sim_clock.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/ui/ui_renderer.hpp"
#include "game/ui/hud.hpp"
#include "game/ui/picking.hpp"
#include "game/visual/render_mode.hpp"
#include "game/visual/vision_overlay_pass.hpp"

namespace badlands {

class GameView : public AppView {
 public:
  explicit GameView(RenderMode mode = RenderMode::Detailed) : mode_(mode) {}
  ~GameView() override;

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  // Jump the day/night clock to an absolute time-of-day (headless capture).
  void SeekToTimeOfDay(float t01) override;
  void DrawUI() override;
  UiRenderer* GetUiRenderer() override { return ui_.get(); }
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
  // Seeds the demo town via sim_.Dispatch(ActionKind::PlaceBuilding) at a
  // few spread-out, non-overlapping tiles around the prebuilt origin Castle.
  void PlaceDemoBuildings();
  // Clears scene_ and rebuilds it from scratch through the visual SceneComposer:
  // re-mirrors scene_context_'s lighting, generates the symbolic greybox map
  // (SymbolicMapGenerator) and adds its terrain chunks + lake water surfaces,
  // then adds every sim_.Buildings() row via AddBuildingToComposer. mode_ picks
  // blockout vs detailed materials. Called once from Initialize. NOTE: the sim
  // ticks, but this stage has no dynamic entities, so the scene stays valid;
  // when dynamic entities land, BuildScene (or an incremental update) must be
  // re-driven from the sim each frame the world changes.
  void BuildScene();

  // GPU handles (from RenderContext, stored so DrawUI can re-bake the sky when
  // a DaylightConfig value changes live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  // Detailed (PBR) vs Blockout (debug) proxy materials, set at construction
  // from the USE_BLOCKOUT_MODE env toggle. Drives which material components the
  // SceneComposer attaches + which water factory / terrain arrays are built.
  RenderMode mode_ = RenderMode::Detailed;

  MaterialLibrary matlib_;
  CubemapBuilder sky_cube_;

  // Forward-transparent water surface material (BuildWaterForwardFactory in
  // detailed mode, BuildWaterBlockoutForwardFactory in blockout mode).
  std::unique_ptr<MaterialInstanceFactory> water_factory_;
  // Terrain-blend layer arrays for the symbolic map (PBR biome packs in detailed
  // mode, DebugTerrainArrays solid colors in blockout mode). Held so the arrays
  // outlive rendering.
  MaterialLibrary::TerrainArrays terrain_arrays_;

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

  // Owns the sim (RAII value member; nullptr script = mock brains).
  badlands::Sim sim_{nullptr};

  // Fog-of-war overlay: the game's ScenePostPass. Registered on scene_context_
  // (post_pass) so both the windowed renderer and the headless --screenshot
  // renderer apply it; fed the sim's VisionField each frame.
  VisionOverlayPass vision_pass_;

  // Reused scratch buffer for sim_.Buildings(building_rows_) (BuildScene + HUD
  // + DrawUI), so the per-frame reads don't allocate a fresh vector each call.
  std::vector<badlands::BuildingState> building_rows_;
  // Same, for sim_.Characters() rows (HUD model + hero picking).
  std::vector<badlands::CharacterState> character_rows_;

  // --- Game UI (NOT the ImGui debug UI; see CLAUDE.md) ---
  // Owned here rather than by the app so views without a HUD pay nothing; the
  // app runs the pass via AppView::GetUiRenderer.
  std::unique_ptr<UiRenderer> ui_;
  HudFrame hud_frame_;  // this frame's quads + hit rects
  // Which entity the detail panel is describing. kNoPick = nothing selected.
  uint32_t selected_building_ = kNoPick;
  uint32_t selected_hero_ = kNoPick;
  // Guild roster cap mirrored from WorldState, for the occupancy row.
  uint32_t roster_cap_ = 0;
  // Cached each RefreshHud() so DrawUI's "World" debug window reuses this
  // frame's snapshot instead of re-reading the sim.
  uint32_t hud_gold_ = 0;
  uint32_t hud_building_total_ = 0;
  // Physical-pixel size of the last frame's surface, for HUD layout + hit
  // testing. Both are physical because ui_build works in physical pixels.
  float ui_viewport_w_ = 0.0f;
  float ui_viewport_h_ = 0.0f;
  float ui_scale_ = 1.0f;

  // Refills building_rows_ from the sim (sized to the LIVE building count so
  // picking never reads a stale tail). Returns the building count.
  uint32_t SnapshotBuildings();
  // Rebuilds hud_frame_ from the sim snapshots + selection. Called each Update.
  void RefreshHud();
  // Dispatches the action a HUD button id maps to. Returns true if it ran.
  bool DispatchHudAction(uint32_t hud_id);

  float dt_ = 0.0f;
};

}  // namespace badlands
