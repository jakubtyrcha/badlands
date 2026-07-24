#pragma once

// Task S2.F: badlands_game's real AppView -- renders the sim's buildings
// (static this stage: no game_tick, no unit spawns) through the shared
// deferred renderer + LightEnvironment, viewed with the shared fixed-angle
// GameCameraController (WASD/arrow XZ pan; F is its first consumer).
// Replaces PlaceholderView for the game executable. Terrain/ploppables/units
// are NOT in this stage -- buildings only.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
#include "engine/rendering/projected_decal.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/ui/ui_renderer.hpp"
#include "game/map/map_data.hpp"
#include "game/ui/hud.hpp"
#include "game/ui/picking.hpp"
#include "game/visual/world_labels.hpp"
#include "game/visual/composite_post_pass.hpp"
#include "game/visual/cone_overlay_pass.hpp"
#include "game/visual/nav_debug_overlay.hpp"
#include "game/visual/render_mode.hpp"
#include "game/visual/selection_decals.hpp"
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
  // Seed a living town on the plains (guilds/tavern/apothecary/houses/sewer/
  // hunter's camp), recruit heroes, and release a deer herd into nearby forest --
  // all through Sim::Dispatch/Spawn, so it is a logged command sequence.
  void SeedTown();
  // Rebuild the live unit capsules from the sim snapshot each frame, placed on
  // the terrain surface and coloured per entity. Cheap (a handful of units).
  void SyncUnits();
  // Triangle soup (kPolygon: pos.xyz + rgba, 3 verts/tri) for the vision-cone
  // debug overlay, built from the current snapshot. Empty when nothing has vision.
  std::vector<float> BuildVisionConeTriangles() const;
  // Terrain height at a world XZ (0 before the map exists). The map is stored
  // corner-origin, so world XZ is shifted by the half extents.
  float GroundAt(float world_x, float world_z) const;
  // Rebuilds decals_ from the validated selection and republishes it on
  // scene_context_. Called each Update AFTER RefreshHud, which is what drops a
  // selection that no longer exists.
  void RefreshSelectionDecals();
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
  // Wall-clock seconds since startup, accumulated from the raw presentation dt
  // (so it ignores game speed AND pause). Feeds SceneContext::real_time_seconds
  // for UI-domain animation -- currently the selection decals' marching ants.
  // Deterministic under headless capture, where the app feeds a fixed step.
  double real_time_seconds_ = 0.0;
  unsigned long long sim_ticks_done_ = 0;
  double rebake_accum_ = 0.0;
  bool force_rebake_ = false;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // Owns the sim (RAII value member; an empty BrainDesc = mock brains).
  badlands::Sim sim_{badlands::BrainDesc{}};

  // Fog-of-war overlay: the game's ScenePostPass. Registered on scene_context_
  // (post_pass) so both the windowed renderer and the headless --screenshot
  // renderer apply it; fed the sim's VisionField each frame.
  VisionOverlayPass vision_pass_;
  ConeOverlayPass cone_pass_;      // vision-cone debug overlay (toggle in DrawUI)
  CompositePostPass post_passes_;  // runs vision then cones behind the one slot

  // Pathfinding debug overlay (shared with the AI sandbox). Draws the navmesh +
  // a click-two-points path through scene_context_.debug_lines; toggled in the
  // "Gameplay Debug" panel next to the vision cones. Ground height comes from the
  // terrain (GroundAt); picks come from the terrain raycast in HandleEvent.
  NavDebugOverlay nav_debug_;

  // Selection highlights: projected decals (a ring under the selected unit, a
  // rounded rect around the selected building), rebuilt every frame and handed
  // to the renderer through scene_context_.decals. Must outlive the frame --
  // hence a member, not a local.
  std::vector<ProjectedDecal> decals_;

  // Reused scratch buffer for sim_.Buildings(building_rows_) (BuildScene + HUD
  // + DrawUI), so the per-frame reads don't allocate a fresh vector each call.
  std::vector<badlands::BuildingState> building_rows_;
  // The frame's single character snapshot, taken once by SyncUnits() and reused
  // by BuildVisionConeTriangles(), RefreshHud() (HUD model), and world picking.
  std::vector<badlands::CharacterState> character_rows_;

  // --- Game UI (NOT the ImGui debug UI; see CLAUDE.md) ---
  // Owned here rather than by the app so views without a HUD pay nothing; the
  // app runs the pass via AppView::GetUiRenderer.
  std::unique_ptr<UiRenderer> ui_;
  HudFrame hud_frame_;  // this frame's quads + hit rects
  // Which entity the detail panel is describing. kNoPick = nothing selected.
  uint32_t selected_building_ = kNoPick;
  uint32_t selected_hero_ = kNoPick;
  // A HUD click on a resident/visitor entry or the hero's home link selects an
  // entity rather than firing an action. Each such element carries an id
  // >= kHudSelectBase; this table maps (id - kHudSelectBase) back to the target,
  // rebuilt every RefreshHud (parallel to the elements emitted into the model).
  struct HudSelectTarget {
    enum class Kind { Building, Hero } kind;
    uint32_t id;
  };
  std::vector<HudSelectTarget> hud_targets_;
  // True when selected_hero_ came from a list click (a visiting/indoors unit is
  // then still inspectable) rather than a world pick (which never selects an
  // indoors unit). Governs how RefreshHud resolves the selected unit.
  bool selected_unit_from_list_ = false;
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

  // --- Floating world labels (names / health bars / damage numbers) ---
  // Drawn to the color buffer over the 3D scene, in the SAME UI pass as the HUD
  // (world-label quads first so the HUD/panel draws on top). Names & bars are
  // stateless (rebuilt from the snapshot each frame); damage numbers are timed
  // labels in the pool, advanced on the real presentation clock.
  WorldLabelPool timed_labels_;
  std::vector<badlands::GameEvent> events_scratch_;  // drained each frame
  std::vector<UiQuad> floating_quads_;  // this frame's world-label quads
  std::vector<UiQuad> ui_quads_;        // floating + HUD quads, one SetQuads
  std::vector<UiQuad> label_run_scratch_;  // reused ui_text_run glyph buffer
  // Display-name caches, double-buffered: `_prev_` holds LAST frame's set so an
  // entity downed/destroyed THIS frame (already gone from the snapshot, but alive
  // last frame) can still be named in the combat log. Both are rebuilt from the
  // live snapshot each frame, so memory stays bounded to ~live-entity count --
  // dead ids are dropped rather than accumulating forever.
  std::unordered_map<uint32_t, std::string> char_names_;
  std::unordered_map<uint32_t, std::string> char_names_prev_;
  // Head heights (size_y), double-buffered like the names, so a lethal hit's
  // floating damage number gets the just-died victim's real height instead of a
  // hardcoded fallback.
  std::unordered_map<uint32_t, float> char_sizes_;
  std::unordered_map<uint32_t, float> char_sizes_prev_;
  std::unordered_map<uint32_t, BuildingKind> building_kinds_;
  std::unordered_map<uint32_t, BuildingKind> building_kinds_prev_;
  // Combat log: a ring buffer of formatted lines (newest last) and how many
  // lines the user has scrolled up from the tail (0 = following the newest).
  std::vector<std::string> combat_log_lines_;
  int combat_log_scroll_ = 0;

  // Drains this frame's sim events: appends combat-log lines and spawns floating
  // damage numbers on the victims. Runs every frame (even headless) so the sim's
  // event buffer never grows unbounded.
  void PumpGameEvents();
  // Appends one combat-log line, capping the ring buffer and keeping the viewed
  // window stable when the user has scrolled up.
  void PushLogLine(std::string line);
  // Display names for a combat-log line, from the caches (last-known if dead).
  std::string EventActorName(uint32_t slot) const;
  std::string EventTargetName(const badlands::GameEvent& e) const;
  // Head height (size_y) of a character slot from the caches (last-known if the
  // victim just died); a small default when never seen.
  float EventActorSize(uint32_t slot) const;
  // Adjusts the combat-log scroll offset by a wheel delta (+ older / - newer).
  void ScrollCombatLog(float wheel_y);
  // Builds the floating world-label quads (names / health bars / damage numbers)
  // from the current snapshot + camera into `out` (physical-pixel UiQuads).
  void BuildFloatingLabels(std::vector<UiQuad>& out);

  // Refills building_rows_ from the sim (sized to the LIVE building count so
  // picking never reads a stale tail). Returns the building count.
  uint32_t SnapshotBuildings();
  // Refills character_rows_ from the sim once per Update, before SyncUnits and
  // RefreshHud (which both read it) and standing in for picking's snapshot too.
  // Reuses the buffer (Characters(out) overload). Returns the character count.
  uint32_t SnapshotCharacters();
  // Rebuilds hud_frame_ from the sim snapshots + selection. Called each Update.
  void RefreshHud();
  // Dispatches the action a HUD button id maps to (sim actions, speed changes,
  // or a selection change for a clickable entity row). Returns true if handled.
  bool DispatchHudAction(uint32_t hud_id);
  // Registers a selection target and returns the HUD element id to tag the
  // clickable row with (>= kHudSelectBase). Called in the same order the rows
  // are emitted into the model, so the id round-trips back to this target.
  uint32_t AddSelectTarget(HudSelectTarget::Kind kind, uint32_t id);

  // The biome/height map, generated once in BuildScene and kept so SyncUnits can
  // seat units on the terrain surface. half_x_/half_z_ convert world XZ (centred
  // on the origin) to the map's corner-origin local coordinates.
  MapData map_;
  float half_x_ = 0.0f;
  float half_z_ = 0.0f;

  // Live unit capsules, rebuilt from character_rows_ each frame (index-free; the
  // handles are destroyed and re-added).
  std::vector<NodeHandle> unit_nodes_;

  float dt_ = 0.0f;
};

}  // namespace badlands
