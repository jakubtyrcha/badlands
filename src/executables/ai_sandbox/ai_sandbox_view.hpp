#pragma once

// badlands_ai_sandbox's AppView: the HOST for a SandboxMode.
//
// The app is a general surface for driving the AI, and it does exactly three
// things: it asks the mode what world to build, it ticks that world, and it
// draws what is in it. Every system at work -- map, buildings, navmesh, brains,
// combat, skills -- is the game's own, running as it does in the real game,
// because nothing in the game knows a mode exists (see sandbox_mode.hpp).
//
// The inspection surface: the panel shows the sim clock (day/night), the mode's
// own status line, every entity's needs + chosen behaviour, and the tail of the
// command log (the trace of record). Everything it draws comes through the
// badlands::Sim snapshot API (Characters / Buildings / World / CommandLog) --
// the view never reaches into the sim's registry.

#include <cstdint>
#include <optional>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // badlands::Sim + snapshot structs
#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/app/sim_clock.hpp"
#include "engine/core/camera.hpp"
#include "engine/core/ray.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "executables/ai_sandbox/sandbox_mode.hpp"
#include "game/visual/nav_debug_overlay.hpp"

namespace badlands {

class AiSandboxView : public AppView {
 public:
  // Takes ownership of the mode it hosts. Never null -- main picks one.
  explicit AiSandboxView(std::unique_ptr<SandboxMode> mode) : mode_(std::move(mode)) {}

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
  // otherwise clobber it with SceneGraph's own defaults) -- same pattern as
  // GameView::ApplyEnvironment.
  void ApplyEnvironment();
  // Builds the world the mode asks for and hands it to the mode to populate,
  // then loads the creature/skill data files over it (initial config, before
  // any tick). Called at startup and again every time the mode asks for a fresh
  // world. The wasm hero brain is loaded once into hero_wasm_ and reused: it is
  // the ONLY hero decision layer, so a world built without it has heroes that
  // idle (see LoadBrainWasm, src/game/brain_asset.hpp).
  void StageWorld();
  // Clears scene_ and rebuilds the STATIC geometry from the sim: re-mirrors
  // scene_context_'s lighting, then adds the floor and a box per
  // Sim::Buildings() row -- walls included, since a wall IS a building.
  // Entities are NOT rebuilt here -- they get a fixed node pool
  // (CreateUnitCapsules) that SyncUnits repositions per frame, so a moving
  // entity costs a transform write rather than a mesh rebuild.
  void BuildScene();
  void AddBuildings();
  // Per-axis half-extent of everything built in the world, for the floor size
  // and the camera framing. Read off the building snapshot, so the host never
  // has to ask the mode how big its world is.
  glm::vec2 WorldHalfExtent();
  // Per-frame: draw a thin box "tracer" for each in-flight projectile.
  void SyncProjectiles();
  // Per-frame: reads the game_state snapshot and moves/hides the capsule pool.
  // Heroes inside a building are hidden (scaled to zero), matching the sim's
  // "don't draw; list in the panel" contract for inside_building_id >= 0.
  void SyncUnits();
  // The inspector: sim clock, per-hero needs/behaviour, and the tail of the
  // command log.
  void DrawInspector();
  // A left-click ground pick (flat arena plane, y = 0) while the nav overlay's
  // pick mode is on: raycasts to the ground and hands the point to nav_debug_.
  void HandleNavPick(const SDL_Event& event);
  // Centers the game camera on the arena origin and picks a height (at
  // GameCameraController's fixed pitch) so the whole arena -- including the
  // wall ring -- stays inside the frustum. The framing is aspect-independent
  // (it uses fixed empirical coefficients, not camera_.aspect), so it is run
  // once from Initialize; OnResize only refreshes camera_.aspect and must NOT
  // re-run this (it resets gamecam_.focus, discarding any WASD pan).
  void FrameCamera();

  // GPU handles (from RenderContext, stored so DrawUI can re-run
  // ApplyEnvironment when the light-environment editor changes env_ live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  // Floor + building + capsule materials are cached solid-color deferred
  // materials from MaterialLibrary::SolidColor (the library owns their 1x1
  // textures), so no per-view texture/sampler handles are needed here.

  // What is being driven. Owns every decision about what world exists and who
  // is in it; this view owns everything about how it is drawn.
  std::unique_ptr<SandboxMode> mode_;

  // The hero brain wasm, read once and reused across restages -- BrainDesc
  // borrows these bytes at each Sim construction.
  std::vector<uint8_t> hero_wasm_;

  // Owns the sim (RAII; no manual destroy). Built in StageWorld.
  badlands::Sim sim_{badlands::BrainDesc{}};

  // Time model (see sim_clock.hpp): real dt * speed -> sim seconds; the sim
  // runs fixed game_ticks up to TickTarget(), so the speed control accelerates
  // the day/night loop without changing the tick rate the sim sees.
  SimClock sim_clock_;
  unsigned long long sim_ticks_done_ = 0;

  // Reused snapshot read-back buffers (no per-frame heap churn).
  std::vector<badlands::CharacterState> char_rows_;
  std::vector<badlands::BuildingState> building_rows_;
  std::vector<badlands::CommandRecord> cmd_rows_;
  uint32_t command_log_total_ = 0;
  // Drained each tick and discarded: this view has no combat log, but the sim's
  // transient event stream must still be emptied or it grows without bound.
  std::vector<badlands::GameEvent> events_scratch_;

  // Fixed pool of hero capsule nodes; index == game_state row index.
  std::vector<NodeHandle> capsule_nodes_;
  // Per-frame projectile tracer nodes (rebuilt each frame; usually few).
  std::vector<NodeHandle> projectile_nodes_;
  std::vector<badlands::ProjectileState> projectile_rows_;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // Pathfinding debug overlay (shared with the game view). Flat arena ground
  // (y = 0); picks come from HandleNavPick's ground-plane raycast.
  NavDebugOverlay nav_debug_;

  float dt_ = 0.0f;
};

}  // namespace badlands
