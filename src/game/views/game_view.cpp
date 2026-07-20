#include "game/views/game_view.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/profiler.hpp"
#include "engine/app/fixed_timestep.hpp"
#include "engine/app/game_camera_controller.hpp"  // ZoomAtCursor
#include "engine/app/sdl_input_util.hpp"           // EventWindowLogicalSize
#include "core/geometry_type.hpp"
#include "engine/rendering/fog_sim.hpp"
#include "engine/rendering/geometry/mesh_builder_utils.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/water_material.hpp"
#include "engine/core/ray.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/building_catalog.h"
#include "game/geometry/terrain_mesh.hpp"
#include "game/geometry/water_surface.hpp"
#include "game/map/symbolic_map_generator.hpp"
#include "game/scene/blockout_materials.hpp"
#include "game/scene/building_composer.hpp"
#include "game/visual/scene_composer.hpp"
#include "mapview/biome_manifest.hpp"

namespace badlands {

namespace {

// rotation_index 0..3 -> 0/45/90/135deg world yaw about Y (badlands_game.h's
// GamePlacementDesc convention) -- see building_scene.h's AddBuildingToScene
// comment for why this is exact for 0/2 and an approximation for 1/3.
float yaw_from_rotation_index(int32_t rotation_index) {
  return glm::radians(static_cast<float>(rotation_index) * 45.0f);
}

// Up to this many rows are read from game_buildings/game_state per call --
// comfortably above this stage's Castle + 4 demo buildings.
constexpr uint32_t kMaxBuildingRows = 64;

// Game-UI text size in LOGICAL pixels. The atlas is baked once at this size, so
// changing it means a re-bake, not a re-layout.
constexpr float kHudFontPx = 18.0f;

// game_state() readback cap for characters. Separate from kMaxBuildingRows
// because it bounds a different population (heroes/monsters, potentially many
// more than buildings): the HUD's roster/visitor counts iterate this snapshot,
// so too small a cap would undercount a busy guild.
constexpr uint32_t kMaxCharacterRows = 256;

// Day/night: the sky cube + SH + IBL prefilter are re-baked at most once per
// this much REAL time (the directional light + shadows still move every frame).
// A real-time throttle bounds the ~per-frame HW cube bake cost independent of
// framerate and sim speed; an Update(0) never re-bakes. Seeks/config edits
// force an immediate re-bake.
constexpr double kRebakeIntervalSeconds = 1.0 / 20.0;  // ~20 Hz sky/IBL refresh

// Real seconds for one in-game day (overrides SimClock's 5-minute default).
// Short for now so the whole cycle is quick to see.
constexpr float kRealSecondsPerDay = 16.0f;

// Dynamic sky/IBL source-cube face resolution. 64 keeps the (throttled, now
// multithreaded) HW bake cheap; the HW sky is smooth so 64 reads fine as the
// skybox background + IBL prefilter source.
constexpr uint32_t kSkyCubeFaceSize = 64;

// Where the live day/night clock starts (t01: 0.30 = mid-morning).
constexpr float kInitialTimeOfDay = 0.30f;

// Biome -> PBR pack manifest (detailed terrain); layer index == Biome enum.
constexpr const char* kBiomeManifestPath =
    "assets/materials/terrain_biomes.json";

// Terrain chunking: N x N lattice cells per mesh entity. Mesh density is
// lattice density (one X-split quad per cell), so there is no subdiv knob.
constexpr int kChunkCells = 16;

// The demo buildings (sim-placed around the origin) are shifted onto the
// southern plains band of the origin-centered symbolic map so they sit on land
// rather than in the central lake. ~one tile south of center.
constexpr float kDemoBuildingsSouthShift = 51.2f;

// Flat water surface mesh from the map's block-aligned lake triangles. Same
// kTexturedMesh layout GenerateLakeSurfaceMesh emits (pos/uv/normal/tangent,
// normal +Y, UV = world XZ), but fed an explicit triangle soup instead of
// rasterizing a polygon -- the generator already decided the shape on the block
// lattice, so there is nothing to rasterize.
TexturedMeshResult BuildLakeMesh(const std::vector<glm::vec3>& tris) {
  TexturedMeshResult result;
  result.mesh.geometry_type = GeometryType::kTexturedMesh;
  const glm::vec3 normal(0.0f, 1.0f, 0.0f);
  const glm::vec3 tangent(1.0f, 0.0f, 0.0f);
  auto& v = result.mesh.vertices;
  v.reserve(tris.size() * kTexturedMeshFloatsPerVertex);
  for (const glm::vec3& p : tris) {
    PushVertex(v, p, glm::vec2(p.x, p.z), normal, tangent);
  }
  result.mesh.vertex_count =
      static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
  result.local_bounds =
      ComputeLocalAabbFromVertices(v, kTexturedMeshFloatsPerVertex);
  return result;
}

}  // namespace

GameView::~GameView() {
  if (game_) {
    game_destroy(game_);
    game_ = nullptr;
  }
}

bool GameView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("GameView::Initialize: MaterialLibrary init failed");
    return false;
  }

  // Mode-appropriate proxy materials for the symbolic map's water + terrain.
  const bool blockout_mode = (mode_ == RenderMode::Blockout);
  water_factory_ =
      blockout_mode
          ? BuildWaterBlockoutForwardFactory(ctx.device, ctx.queue, ctx.pipeline_gen)
          : BuildWaterForwardFactory(ctx.device, ctx.queue, ctx.pipeline_gen);
  if (!water_factory_) {
    spdlog::error("GameView::Initialize: water factory build failed");
    return false;
  }

  if (blockout_mode) {
    // Debug solid-color terrain layers, indexed by biome enum value.
    terrain_arrays_ = matlib_.DebugTerrainArrays(blockout::BiomeColors());
  } else {
    // PBR biome packs (same manifest the map tool uses); layer index == biome.
    std::vector<std::string> pack_dirs;
    if (!ResolveBiomePacks(kBiomeManifestPath, pack_dirs)) {
      spdlog::error("GameView::Initialize: failed to resolve biome packs");
      return false;
    }
    terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  }
  if (!matlib_.ok()) {
    spdlog::error("GameView::Initialize: terrain arrays failed to build");
    return false;
  }

  // Game UI (the in-world HUD, NOT the ImGui debug UI -- see CLAUDE.md). Owned
  // by the view like the material factories above; the app runs its pass via
  // GetUiRenderer(). A HUD failure is not fatal: the game is still playable
  // without it, and DrawUI's debug windows still show the same state.
  ui_ = std::make_unique<UiRenderer>();
  if (!ui_->Initialize(ctx.device, ctx.queue, *ctx.pipeline_gen,
                       ctx.surface_format,
                       "assets/fonts/CormorantUnicase-Regular.ttf",
                       kHudFontPx, /*scale_factor=*/1.0f)) {
    spdlog::warn("GameView::Initialize: game UI disabled (font/pipeline setup failed)");
    ui_.reset();
  }

  // Seed the day/night cycle: set the day length, then jump to the initial
  // time-of-day (bakes the sky/IBL/ambient + sets the sun into scene_context_
  // before BuildScene mirrors it).
  sim_clock_.real_seconds_per_day = kRealSecondsPerDay;
  SeekToTimeOfDay(kInitialTimeOfDay);

  // nullptr brain_script_source: mock brains only (no noiser script needed
  // for a static-buildings scaffold) -- game_create also prebuilds the
  // Castle at the origin.
  game_ = game_create(nullptr);
  PlaceDemoBuildings();
  BuildScene();
  // Water test map: keep the frame clear (no volumetric fog obscuring the lake).
  if (scene_renderer_) scene_renderer_->MutableFogConfig().enabled = false;

  // Fixed-angle overview of the 256 m symbolic map (centered on the origin):
  // a fairly top-down pitch keeps the whole biome layout + central lake legible;
  // scroll to zoom in on the demo buildings on the southern plains.
  gamecam_.focus = glm::vec3(0.0f, 0.0f, 0.0f);
  gamecam_.pitch_deg = 64.0f;
  gamecam_.height = 175.0f;
  gamecam_.UpdateCamera(camera_);

  if (!matlib_.ok()) {
    spdlog::error("GameView::Initialize: material pack(s) failed to load");
    return false;
  }
  return true;
}

void GameView::UpdateDaylight() {
  const DaylightState state = ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());

  // Cheap, every frame: move the directional light + shadows. (SetSunColor
  // only sets sun_color_, so it does not disturb the SH ambient between
  // re-bakes -- see scene_graph.cpp.)
  scene_.SetSunDirection(state.light_direction);
  scene_.SetSunColor(state.light_color * state.light_intensity);

  // Expensive HW sky cube + SH + IBL: throttled by REAL time (rebake_accum_ is
  // advanced by the caller), or forced by a DaylightConfig edit. Because the
  // throttle keys on real time (not a frame counter), an Update(0) -- e.g. the
  // recorder re-rendering a captured frame -- never triggers a redundant bake.
  if (force_rebake_ || rebake_accum_ >= kRebakeIntervalSeconds) {
    RebakeSky(state);
    rebake_accum_ = 0.0;
    force_rebake_ = false;
  }
}

void GameView::ApplyDaylightNow() {
  // Unconditional recompute + re-bake for the current time-of-day (init / seek).
  const DaylightState state = ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());
  scene_.SetSunDirection(state.light_direction);
  scene_.SetSunColor(state.light_color * state.light_intensity);
  RebakeSky(state);
  rebake_accum_ = 0.0;
}

void GameView::RebakeSky(const DaylightState& state) {
  ApplyDaylightEnvironment(state, daylight_cfg_, device_, queue_, sky_cube_,
                           scene_context_, kSkyCubeFaceSize);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void GameView::PlaceDemoBuildings() {
  struct Demo {
    GameBuildingKind kind;
    float x, z;
    int32_t rotation_index;
  };
  // Well clear of the origin Castle's 4x4 footprint + 1-tile margin, and
  // spaced >= 8 world units apart so no demo building's footprint + margin
  // can overlap another's regardless of rotation.
  constexpr Demo kDemo[] = {
      {GAME_BUILDING_FREE_COMPANY_QUARTERS, 12.0f, 0.0f, 0},
      {GAME_BUILDING_TAVERN, 12.0f, 10.0f, 2},  // rotated 90deg -- proves yaw wiring
      {GAME_BUILDING_WATCHTOWER, -12.0f, 0.0f, 0},
      {GAME_BUILDING_APOTHECARY, -12.0f, 10.0f, 0},
  };
  for (const Demo& d : kDemo) {
    GameAction action{
        .kind = GAME_ACTION_PLACE_BUILDING,
        .target_id = 0,
        .world_x = d.x,
        .world_z = d.z,
        .param_a = static_cast<int32_t>(d.kind),
        .param_b = d.rotation_index,
    };
    const int64_t id = game_dispatch(game_, &action);
    if (id < 0) {
      spdlog::error(
          "GameView::PlaceDemoBuildings: placement failed for kind={} at "
          "({}, {}) rot={}",
          static_cast<int32_t>(d.kind), d.x, d.z, d.rotation_index);
    }
  }
}

void GameView::BuildScene() {
  // NOTE(lighting): the SceneGraph() reset + Set{SunDirection,SunColor,
  // AmbientSH} sequence below mirrors scene_context_'s already-derived
  // lighting (from the daylight bake in SeekToTimeOfDay/UpdateDaylight) into
  // the fresh graph (whose constructor otherwise resets sun/ambient to
  // SceneGraph's own defaults, which the per-frame SyncToRegistry would then
  // write back over scene_context_).
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  SceneComposer composer(mode_);

  // Generate the symbolic greybox map and center it on the world origin (map
  // coordinates are corner-origin, so shift the whole map by -size/2).
  const MapData map = SymbolicMapGenerator{}.Generate();
  const float half_x = map.size_x_m() * 0.5f;
  const float half_z = map.size_z_m() * 0.5f;
  const glm::mat4 center =
      glm::translate(glm::mat4(1.0f), glm::vec3(-half_x, 0.0f, -half_z));

  // Terrain: one kTerrainBlend chunk entity per N x N lattice-cell region.
  const int cells_x = map.nodes_x() - 1;
  const int cells_z = map.nodes_z() - 1;
  for (int cz = 0; cz < cells_z; cz += kChunkCells) {
    for (int cx = 0; cx < cells_x; cx += kChunkCells) {
      TerrainMeshParams p;
      p.cell_x0 = cx;
      p.cell_z0 = cz;
      p.cells_x = std::min(kChunkCells, cells_x - cx);
      p.cells_z = std::min(kChunkCells, cells_z - cz);
      TerrainMesh chunk = BuildTerrainMesh(map, p);
      if (chunk.vertex_count == 0) continue;
      composer.AddTerrain(std::move(chunk), center);
    }
  }

  // Water surface, derived from the map's Lake-dominant cells (the frozen map
  // contract carries no geometry) -- it lands on the same X-split lattice as
  // the terrain, already at the map's water level.
  const InstanceParams water_params = (mode_ == RenderMode::Blockout)
                                          ? BlockoutWaterParams()
                                          : DefaultWaterParams();
  auto lake = BuildLakeMesh(BuildWaterSurfaceTriangles(map));
  if (lake.mesh.vertex_count > 0) {
    composer.AddWater(std::move(lake), water_params, center);
  }

  // Demo buildings from the sim, shifted onto the southern plains band (so they
  // sit on land, not the central lake) and placed through the composer so they
  // honor the render mode's materials.
  if (building_rows_.size() < kMaxBuildingRows) {
    building_rows_.resize(kMaxBuildingRows);
  }
  const uint32_t total = SnapshotBuildings();
  const uint32_t count = static_cast<uint32_t>(building_rows_.size());
  for (uint32_t i = 0; i < count; ++i) {
    const GameBuildingState& b = building_rows_[i];
    const glm::vec2 world(b.center_x, b.center_z + kDemoBuildingsSouthShift);
    // Ground height straight from the map's query API (world -> map-local).
    const float ground = map.HeightAt(world.x + half_x, world.y + half_z);
    AddBuildingToComposer(composer, static_cast<GameBuildingKind>(b.kind), world,
                          yaw_from_rotation_index(b.rotation_index), ground);
  }

  composer.ComposeInto(scene_, matlib_, terrain_arrays_, water_factory_.get());
}

void GameView::HandleEvent(const SDL_Event& event, int width, int height) {
  // Fixed-angle camera: only zoom is mouse-driven (wheel + trackpad, which SDL
  // reports as the same event with fractional deltas). Key panning is read
  // directly from Update()'s keyboard_state snapshot instead of per-event.
  //
  // Use the window's LOGICAL size (EventWindowLogicalSize), not HandleEvent's
  // physical-pixel width/height: SDL mouse coords are points, so mixing them
  // with a pixel extent scales the anchor ray off the cursor on HiDPI displays.
  //
  // ImGui capture is gated PER INPUT KIND: WantCaptureMouse must not swallow
  // keyboard events (else ESC-to-deselect dies whenever a debug window is under
  // the cursor) and vice versa.
  const ImGuiIO& io = ImGui::GetIO();

  switch (event.type) {
    case SDL_EVENT_MOUSE_WHEEL: {
      if (io.WantCaptureMouse) return;
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
      ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel),
                   glm::vec2(event.wheel.mouse_x, event.wheel.mouse_y), screen);
      return;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (io.WantCaptureMouse) return;
      if (event.button.button != SDL_BUTTON_LEFT) return;
      glm::vec2 logical;
      if (!EventWindowLogicalSize(event.button.windowID, logical)) return;

      // ONE click, TWO coordinate spaces. SDL reports the cursor in logical
      // points; the ray math wants points against a point extent, while the
      // HUD's hit rects came back from ui_build in PHYSICAL pixels. Mixing
      // them is invisible at 1x and wrong on every HiDPI display -- i.e. on
      // the dev machine -- so convert explicitly for the HUD test.
      const glm::vec2 cursor_logical(event.button.x, event.button.y);
      const glm::vec2 to_physical(
          logical.x > 0.0f ? static_cast<float>(width) / logical.x : 1.0f,
          logical.y > 0.0f ? static_cast<float>(height) / logical.y : 1.0f);
      const glm::vec2 cursor_physical = cursor_logical * to_physical;

      // Input priority: ImGui (handled above) -> game UI -> world -> pan.
      const uint32_t hud_id =
          HudHitTest(hud_frame_, cursor_physical.x, cursor_physical.y);
      if (hud_id != kHudNone) {
        // Any hit on HUD chrome is CONSUMED, even a disabled button or the
        // panel background -- otherwise the click falls through and deselects
        // the very entity the panel is describing.
        if (!HudHitIsDisabled(hud_frame_, cursor_physical.x,
                              cursor_physical.y)) {
          DispatchHudAction(hud_id);
        }
        return;
      }

      // World pick: ray against the ground plane, then the drawn footprints.
      const Ray ray = ScreenPointToRay(camera_, cursor_logical, logical);
      glm::vec3 hit;
      if (!IntersectGroundPlane(ray, 0.0f, hit)) return;  // at/above the horizon

      // BuildScene renders the demo buildings shifted south by
      // kDemoBuildingsSouthShift, but building_rows_ (and BuildingAtWorld)
      // are in the sim's UNSHIFTED space. Undo the render shift so the pick
      // space matches the sim space -- without this every building is
      // unclickable. Keep this the inverse of the shift applied in BuildScene.
      const glm::vec2 sim_world(hit.x, hit.z - kDemoBuildingsSouthShift);

      // NOTE: heroes are NOT pickable in this stage. BuildScene composes only
      // terrain, water, and buildings -- no character/hero geometry is drawn --
      // so a hero pick would select an entity invisible on screen. HeroAtWorld
      // and the hero HUD branch are kept, ready to wire up once heroes render.
      selected_hero_ = kNoPick;
      selected_building_ =
          BuildingAtWorld(building_rows_.data(),
                          static_cast<uint32_t>(building_rows_.size()), sim_world);
      return;
    }

    case SDL_EVENT_KEY_DOWN:
      if (io.WantCaptureKeyboard) return;
      if (event.key.key == SDLK_ESCAPE) {
        selected_building_ = kNoPick;
        selected_hero_ = kNoPick;
      }
      return;

    default:
      return;
  }
}

bool GameView::DispatchHudAction(uint32_t hud_id) {
  if (!game_) return false;
  GameAction action{};
  switch (hud_id) {
    case kHudBtnRecruit:
      if (selected_building_ == kNoPick) return false;
      action.kind = GAME_ACTION_RECRUIT_HERO;
      action.target_id = selected_building_;
      break;
    case kHudBtnDestroy:
      if (selected_building_ == kNoPick) return false;
      action.kind = GAME_ACTION_DESTROY_BUILDING;
      action.target_id = selected_building_;
      break;
    default:
      return false;  // panel/bar background: consumed, but not an action
  }
  const int64_t rc = game_dispatch(game_, &action);
  if (rc < 0) {
    spdlog::warn("GameView: action {} on {} rejected ({})", hud_id,
                 selected_building_, rc);
    return false;
  }
  if (hud_id == kHudBtnDestroy) {
    // The building is gone; nothing left for the panel to describe.
    selected_building_ = kNoPick;
    BuildScene();  // footprints changed -> the scene must be rebuilt
  }
  return true;
}

void GameView::Update(float dt, const bool* keyboard_state) {
  PROFILE_SCOPE("GameView::Update");
  dt_ = dt;

  // Advance the single time source: real dt -> * sim speed -> sim time. Both
  // the day/night cycle and the fixed-rate game logic derive from this clock,
  // so they run together at the current speed, independent of framerate.
  const double real_dt = static_cast<double>(dt);
  const double sim_dt = sim_clock_.Advance(real_dt);

  // Feed the presentation clock to time-animated forward materials (water
  // waves). Deterministic under headless SeekToTimeOfDay (sim_seconds is set
  // deterministically from t01), so screenshots/records are reproducible.
  scene_context_.time_seconds = static_cast<float>(sim_clock_.sim_seconds);

  // The map fog generator is a sim: advance it on sim-time so pause freezes it
  // and game speed scales it. The renderer flushes the accumulated time into
  // fixed steps during Render (which owns the GPU encoder).
  if (scene_renderer_) {
    scene_renderer_->GetFogSimulation().AddTime(static_cast<float>(sim_dt));
  }

  // Fixed-interval game logic: run game_tick(kTickDt) until we've caught up to
  // the clock's tick target. Bounded (real dt is clamped in Advance); the
  // budget is pure spiral-of-death safety. Ticks scale with sim speed because
  // the target is derived from sim time.
  {
    PROFILE_SCOPE("game_ticks");
    const unsigned long long tick_target = sim_clock_.TickTarget();
    int budget = kMaxSimTicksPerFrame;
    while (sim_ticks_done_ < tick_target && budget-- > 0) {
      if (game_) game_tick(game_, static_cast<float>(kTickDt));
      ++sim_ticks_done_;
    }
  }

  // Day/night rendering, driven by the clock's time-of-day. The sky re-bake is
  // throttled by real time (accumulated here); the directional light moves
  // every frame inside UpdateDaylight.
  rebake_accum_ += real_dt;
  {
    PROFILE_SCOPE("daylight");
    UpdateDaylight();
  }

  // ImGui context guard: Update() runs even in --screenshot mode, where no
  // ImGui context exists (SdlViewerApp only calls InitImGui() for the
  // windowed loop) -- ImGui::GetIO() asserts without a current context.
  if (keyboard_state != nullptr && ImGui::GetCurrentContext() != nullptr &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    glm::vec2 dir(0.0f);
    if (keyboard_state[SDL_SCANCODE_W] || keyboard_state[SDL_SCANCODE_UP]) dir.y -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_S] || keyboard_state[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    if (keyboard_state[SDL_SCANCODE_A] || keyboard_state[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_D] || keyboard_state[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
    gamecam_.PanKeyboard(dir, dt);  // zoom-scaled; no-op when dir is zero
  }

  gamecam_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);

  {
    PROFILE_SCOPE("hud");
    RefreshHud();
  }
}

uint32_t GameView::SnapshotBuildings() {
  if (!game_) {
    building_rows_.clear();
    return 0;
  }
  if (building_rows_.size() < kMaxBuildingRows) {
    building_rows_.resize(kMaxBuildingRows);
  }
  const uint32_t total =
      game_buildings(game_, building_rows_.data(), kMaxBuildingRows);
  // Size to the LIVE count, never the capacity: picking reads
  // building_rows_.size(), so a padded tail would let a click land on a stale
  // row (e.g. after a Destroy, before the next refresh).
  building_rows_.resize(std::min(total, kMaxBuildingRows));
  return total;
}

void GameView::RefreshHud() {
  if (!game_) return;

  // Snapshot the sim into the reused row buffers. These back BOTH the HUD model
  // and picking, so what the panel describes is exactly what a click can hit.
  // Update() runs this before DrawUI() the same frame, so DrawUI reuses these
  // rows + the cached scalars below instead of re-reading them from the sim.
  hud_building_total_ = SnapshotBuildings();
  if (character_rows_.size() < kMaxCharacterRows) {
    character_rows_.resize(kMaxCharacterRows);
  }
  const uint32_t character_total =
      game_state(game_, character_rows_.data(), kMaxCharacterRows);
  character_rows_.resize(std::min(character_total, kMaxCharacterRows));

  GameWorldState world{};
  game_world(game_, &world);
  roster_cap_ = world.guild_roster_cap;
  hud_gold_ = world.gold;

  HudModel model;
  model.gold = world.gold;
  {
    // Time of day from the presentation clock -- it is NOT in the game C ABI.
    const float t01 = sim_clock_.TimeOfDay();
    const int minutes = static_cast<int>(t01 * 24.0f * 60.0f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Day %llu   %02d:%02d",
                  static_cast<unsigned long long>(sim_clock_.DayCounter()) + 1,
                  (minutes / 60) % 24, minutes % 60);
    model.clock_text = buf;
  }

  // A selection that no longer exists (destroyed, or a hero that entered a
  // building) silently clears rather than describing a stale row.
  const GameBuildingState* selected = nullptr;
  for (const GameBuildingState& b : building_rows_) {
    if (b.id == selected_building_) selected = &b;
  }
  const GameCharacterState* hero = nullptr;
  for (const GameCharacterState& c : character_rows_) {
    if (c.id == selected_hero_) hero = &c;
  }
  if (!selected) selected_building_ = kNoPick;
  if (!hero) selected_hero_ = kNoPick;

  if (selected) {
    const GameBuildingKind kind = static_cast<GameBuildingKind>(selected->kind);
    const GameBuildingDef def = game_building_def(selected->kind);

    // Occupancy: heroes whose home guild is this building.
    uint32_t occupancy = 0;
    uint32_t visitors = 0;
    for (const GameCharacterState& c : character_rows_) {
      if (c.home_building_id == static_cast<int32_t>(selected->id)) ++occupancy;
      if (c.inside_building_id == static_cast<int32_t>(selected->id)) ++visitors;
    }

    HudSelection s;
    s.kind = HudSelection::Kind::Building;
    s.id = selected->id;
    s.title = building_label(kind);
    s.rows.emplace_back("id", "#" + std::to_string(selected->id));
    {
      char pos[48];
      std::snprintf(pos, sizeof(pos), "%.0f, %.0f", selected->center_x,
                    selected->center_z);
      s.rows.emplace_back("position", pos);
    }
    s.rows.emplace_back("footprint",
                        std::to_string(selected->width_tiles) + " x " +
                            std::to_string(selected->depth_tiles));
    // A guild is a building that can recruit; only those show a roster row.
    // Only actual guilds (the 4 recruiting kinds) show a roster + Recruit
    // button. Gating on def.recruits -- the sim's own classification -- means
    // the button is never shown enabled for a building game_dispatch would
    // reject (Castle/Tavern/Watchtower/Apothecary are not guilds).
    const bool is_guild = def.recruits != 0;
    if (is_guild) {
      s.rows.emplace_back("roster", std::to_string(occupancy) + " / " +
                                        std::to_string(roster_cap_));
      s.show_recruit = true;
      s.can_recruit = occupancy < roster_cap_;
    }
    if (visitors > 0) s.rows.emplace_back("inside", std::to_string(visitors));
    s.show_destroy = def.user_destructible != 0;
    s.can_destroy = s.show_destroy;

    model.has_selection = true;
    model.selection = std::move(s);
  } else if (hero) {
    HudSelection s;
    s.kind = HudSelection::Kind::Hero;
    s.id = hero->id;
    s.title = "Hero";
    s.rows.emplace_back("id", "#" + std::to_string(hero->id));
    {
      char hp[48];
      std::snprintf(hp, sizeof(hp), "%.0f / %.0f", hero->hp, hero->max_hp);
      s.rows.emplace_back("health", hp);
    }
    {
      char pos[48];
      std::snprintf(pos, sizeof(pos), "%.0f, %.0f", hero->pos_x, hero->pos_z);
      s.rows.emplace_back("position", pos);
    }
    if (hero->home_building_id >= 0) {
      s.rows.emplace_back("guild", "#" + std::to_string(hero->home_building_id));
    }
    model.has_selection = true;
    model.selection = std::move(s);
  }

  if (!ui_ || ui_viewport_w_ <= 0.0f) return;
  BuildHud(ui_->context(), model, ui_viewport_w_, ui_viewport_h_, ui_scale_,
           hud_frame_);
  ui_->SetQuads(hud_frame_.quads.data(),
                static_cast<uint32_t>(hud_frame_.quads.size()));
}

void GameView::SeekToTimeOfDay(float t01) {
  // Discontinuous jump (headless capture / editor scrub): move the clock and
  // sync the game-tick counter so we do NOT simulate the skipped time, then
  // re-bake immediately so a single frame is correct.
  sim_clock_.SeekTimeOfDay(t01);
  sim_ticks_done_ = sim_clock_.TickTarget();
  scene_context_.time_seconds = static_cast<float>(sim_clock_.sim_seconds);
  ApplyDaylightNow();
}

void GameView::DrawUI() {
  if (!scene_renderer_) return;

  // Daylight + sim-time controls + the shared renderer-debug selectors + FPS.
  // Editing a DaylightConfig value forces an immediate sky re-bake so the
  // change is visible without waiting for the throttle.
  ImGui::Begin("Daylight / Sim");

  // --- Simulation (time) ---
  if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawSimClockControls(sim_clock_)) {
      SeekToTimeOfDay(sim_clock_.TimeOfDay());  // re-sync tick counter + re-bake
    }
  }

  // --- Directional light (daylight) ---
  if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawDaylightEditor(daylight_cfg_)) force_rebake_ = true;
  }

  // --- Fog (self-contained collapsing section) ---
  EditorUI::DrawFogEditor(*scene_renderer_);

  // --- Debug views ---
  if (ImGui::CollapsingHeader("Debug Views")) {
    EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
    EditorUI::DrawShadowDebugSelector(*scene_renderer_);
  }
  EditorUI::DrawStats(dt_);
  ImGui::End();

  if (!game_) return;

  // Reuse this frame's snapshot: Update()->RefreshHud() already read gold and
  // filled building_rows_ (sized to the live count) just before DrawUI runs,
  // so there is no need to round-trip the sim again here.
  const uint32_t count = static_cast<uint32_t>(building_rows_.size());

  ImGui::Begin("World");
  ImGui::Text("Gold: %u", hud_gold_);
  ImGui::Text("Buildings: %u", hud_building_total_);
  ImGui::Separator();
  for (uint32_t i = 0; i < count; ++i) {
    const GameBuildingState& b = building_rows_[i];
    ImGui::Text("#%u %-24s (%.1f, %.1f)", b.id,
               building_label(static_cast<GameBuildingKind>(b.kind)),
               b.center_x, b.center_z);
  }
  ImGui::End();
}

void GameView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

  // PHYSICAL pixels -- ui_build works in physical pixels throughout, and so do
  // the hit rects it returns.
  ui_viewport_w_ = static_cast<float>(width);
  ui_viewport_h_ = static_cast<float>(height);

  // ui_scale_ (logical->physical for HUD layout) is pinned to 1.0 because the
  // glyph atlas is baked ONCE at kHudFontPx * 1.0 (Initialize) with no re-bake
  // path: driving layout at a >1 density while the atlas stays 1x would render
  // glyphs at half the intended size inside doubled chrome. The window is
  // created WITHOUT SDL_WINDOW_HIGH_PIXEL_DENSITY, so the drawable is the
  // logical size (density 1.0) and this is exact. It also keeps the fixed-size
  // --screenshot capture correct (which would otherwise be laid out at the
  // live window's density into a 1x-sized target). When true HiDPI lands, the
  // atlas bake scale and ui_scale_ must move together (bake at the density and
  // re-bake on change).
  ui_scale_ = 1.0f;
}

}  // namespace badlands
