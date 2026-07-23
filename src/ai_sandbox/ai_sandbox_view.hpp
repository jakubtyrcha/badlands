#pragma once

// badlands_ai_sandbox's AppView: a LIVE view of the headless sim. It owns a
// BadlandsGame, seeds a small town (guild + tavern + apothecary) and a hero
// roster through the command layer, ticks the sim at a fixed timestep with a
// time-acceleration control, and rebuilds blockout geometry from the sim's
// snapshots -- boxes for buildings, capsules for heroes (the arena floor +
// wall ring from game/arena.h stay as the greybox ground).
//
// This is the inspection surface for the game-systems architecture: the panel
// shows the sim clock (day/night), every hero's needs + chosen behaviour, and
// the tail of the command log (the trace of record). Everything it draws comes
// through the badlands::Sim snapshot API (Characters / Buildings / World /
// CommandLog) -- the view never reaches into the sim's registry.

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
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "game/arena.h"
#include "game/scenario.h"

namespace badlands {

class AiSandboxView : public AppView {
 public:
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
  // Creates the sim and seeds the town: places the guild/tavern/apothecary and
  // recruits a hero roster, all through game_dispatch -- i.e. as logged player
  // Commands, so the seed shows up in the command log like anything else. The
  // noiser hero brain is loaded from BADLANDS_BRAIN_SCRIPT (or the default
  // path) when readable; otherwise the sim runs the C++ town brain.
  void SeedTown();
  // Clears scene_ and rebuilds the STATIC geometry from the sim: re-mirrors
  // scene_context_'s lighting, then adds the floor, the wall ring, and a box
  // per game_buildings() row. Heroes are NOT rebuilt here -- they get a fixed
  // node pool (CreateUnitCapsules) that SyncUnits repositions per frame, so a
  // moving hero costs a transform write rather than a mesh rebuild.
  void BuildScene();
  void AddWalls();
  void AddBuildings();
  // Per-frame: draw a thin box "tracer" for each in-flight projectile.
  void SyncProjectiles();
  // Per-frame: reads the game_state snapshot and moves/hides the capsule pool.
  // Heroes inside a building are hidden (scaled to zero), matching the sim's
  // "don't draw; list in the panel" contract for inside_building_id >= 0.
  void SyncUnits();
  // The inspector: sim clock, per-hero needs/behaviour, noiser bug count, and
  // the tail of the command log.
  void DrawInspector();
  // Pathfinding debug overlay: rebuilds nav_lines_ from the navmesh (cells
  // coloured by terrain cost, obstacles distinct) + the picked path, and points
  // scene_context_.debug_lines at it. Run at the end of Update().
  void UpdateNavDebug();
  // The nav debug panel: toggles + the picked path's cost/reachability.
  void DrawNavPanel();
  // A ground pick (left click) while pick mode is on: sets endpoint A then B.
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

  Arena arena_;

  // The loaded scenario (a walled arena + creatures, or empty => the town seed).
  Scenario scenario_;
  bool scenario_is_arena_ = false;
  bool scenario_load_error_ = false;  // a requested scenario failed to parse

  // Owns the sim (RAII; no manual destroy). Seeded in SeedTown.
  badlands::Sim sim_{nullptr};

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

  // Pathfinding debug overlay (game/src/navmesh). Off by default; drawn through
  // the engine's generic debug-line pass via scene_context_.debug_lines.
  DebugLineBuffer nav_lines_;
  std::vector<badlands::NavDebugCell> nav_cells_;  // reused snapshot buffer
  bool nav_show_mesh_ = false;
  bool nav_pick_mode_ = false;
  std::optional<glm::vec2> nav_a_;  // path endpoints (world XZ), picked on ground
  std::optional<glm::vec2> nav_b_;
  badlands::NavPathResult nav_path_;  // last query result (cost / reachable)

  float dt_ = 0.0f;
};

}  // namespace badlands
