#include "game/views/game_view.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
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
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/water_material.hpp"
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

GameView::~GameView() = default;

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

  // Seed the day/night cycle: set the day length, then jump to the initial
  // time-of-day (bakes the sky/IBL/ambient + sets the sun into scene_context_
  // before BuildScene mirrors it).
  sim_clock_.real_seconds_per_day = kRealSecondsPerDay;
  SeekToTimeOfDay(kInitialTimeOfDay);

  // sim_ is constructed with a nullptr brain_script_source (mock brains only;
  // no noiser script needed for a static-buildings scaffold) -- construction
  // also prebuilds the Castle at the origin.
  SeedTown();
  BuildScene();
  // Water test map: keep the frame clear (no volumetric fog obscuring the lake).
  if (scene_renderer_) scene_renderer_->MutableFogConfig().enabled = false;

  // Frame the living town on the southern plains (where SeedTown placed it), not
  // the empty origin lake -- close enough that the units read as characters.
  // Scroll to zoom out to the whole 256 m map; WASD to roam.
  gamecam_.focus = glm::vec3(0.0f, 0.0f, 52.0f);
  gamecam_.pitch_deg = 58.0f;
  gamecam_.height = 70.0f;
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

void GameView::SeedTown() {
  // The town sits on the southern plains band (world z ~ +50), which is land --
  // so buildings render where the sim places them, no shift. (The prebuilt
  // origin Castle is out in the central lake; the town's Watchtower is the
  // on-land deposit point the tax collector actually uses.)
  struct Town {
    BuildingKind kind;
    float x, z;
    int32_t rotation_index;
  };
  constexpr Town kTown[] = {
      {BuildingKind::FreeCompanyQuarters, -16.0f, 48.0f, 0},
      {BuildingKind::HuntersCamp, 16.0f, 48.0f, 0},
      {BuildingKind::Tavern, -16.0f, 60.0f, 0},
      {BuildingKind::Apothecary, 16.0f, 60.0f, 0},
      {BuildingKind::Watchtower, 0.0f, 42.0f, 0},
      {BuildingKind::House, -6.0f, 68.0f, 0},
      {BuildingKind::House, 6.0f, 68.0f, 0},
      {BuildingKind::Sewer, 28.0f, 56.0f, 0},
  };
  for (const Town& t : kTown) {
    Action place{.kind = ActionKind::PlaceBuilding,
                 .target_id = 0,
                 .world_x = t.x,
                 .world_z = t.z,
                 .param_a = static_cast<int32_t>(t.kind),
                 .param_b = t.rotation_index};
    const int64_t id = sim_.Dispatch(place);
    if (id < 0) {
      spdlog::warn("GameView::SeedTown: placing kind {} at ({}, {}) failed",
                   static_cast<int32_t>(t.kind), t.x, t.z);
      continue;
    }
    // Recruit a few from each guild (Free Company + Hunter's Camp).
    if (t.kind == BuildingKind::FreeCompanyQuarters ||
        t.kind == BuildingKind::HuntersCamp) {
      for (int i = 0; i < 3; ++i) {
        Action recruit{.kind = ActionKind::RecruitHero,
                       .target_id = static_cast<uint32_t>(id),
                       .world_x = 0.0f,
                       .world_z = 0.0f,
                       .param_a = 0,
                       .param_b = 0};
        sim_.Dispatch(recruit);
      }
    }
  }

  // A deer herd in the forest near the town, so the hunter has prey in reach.
  // Golden-angle search outward from the town centre for Forest spots.
  const glm::vec2 town_center{0.0f, 56.0f};
  int deer = 0;
  for (int i = 0; i < 600 && deer < 6; ++i) {
    const float ang = static_cast<float>(i) * 2.399963f;
    const float rad = 12.0f + static_cast<float>(i % 30) * 2.0f;
    const glm::vec2 p = town_center + glm::vec2{std::cos(ang) * rad, std::sin(ang) * rad};
    if (sim_.BiomeAt(p.x, p.y) != 2 /* mapgen::Biome::Forest */) {
      continue;
    }
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = p.x;
    d.pos_z = p.y;
    d.team = 2;
    d.hp = 8.0f;
    d.move_speed = 3.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_z = 0.7f;
    d.size_y = 1.0f;
    d.color_r = 0.62f;
    d.color_g = 0.42f;
    d.color_b = 0.20f;
    sim_.Spawn(d);
    ++deer;
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

  // Generate the symbolic map once and keep it (SyncUnits seats units on it).
  // Map coordinates are corner-origin, so center it on the world origin by
  // shifting the whole map by -size/2. World XZ -> map-local is world + half.
  map_ = SymbolicMapGenerator{}.Generate();
  const MapData& map = map_;
  const float half_x = map.size_x_m() * 0.5f;
  const float half_z = map.size_z_m() * 0.5f;
  half_x_ = half_x;
  half_z_ = half_z;
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

  // Buildings at their true sim positions (SeedTown placed the town on the
  // plains, so no render shift is needed) -- sim and render coordinates match,
  // which is what lets the biome-aware AI and its capsules line up with the
  // terrain they reason about.
  sim_.Buildings(building_rows_);
  for (const BuildingState& b : building_rows_) {
    const glm::vec2 world(b.center_x, b.center_z);
    const float ground = map.HeightAt(world.x + half_x, world.y + half_z);
    AddBuildingToComposer(composer, b.kind, world,
                          yaw_from_rotation_index(b.rotation_index), ground);
  }

  composer.ComposeInto(scene_, matlib_, terrain_arrays_, water_factory_.get());
  unit_nodes_.clear();  // ComposeInto rebuilt scene_; the unit pool is gone with it
}

void GameView::SyncUnits() {
  // Rebuild the live unit capsules from the snapshot each frame, coloured per
  // entity and seated on the terrain surface. A handful of units, so a rebuild
  // (rather than a recoloured fixed pool) is the simple, correct choice; hidden
  // (inside-building) units are skipped, matching the sim's snapshot contract.
  for (NodeHandle n : unit_nodes_) {
    scene_.DestroyNode(n);
  }
  unit_nodes_.clear();

  char_rows_ = sim_.Characters();
  int index = 0;
  for (const CharacterState& c : char_rows_) {
    if (c.inside_building_id >= 0) {
      continue;  // hidden
    }
    const float ground = map_.empty()
                             ? 0.0f
                             : map_.HeightAt(c.pos_x + half_x_, c.pos_z + half_z_);
    const float radius = 0.5f * std::min(c.size_x, c.size_z);
    const float cyl = std::max(0.1f, c.size_y - 2.0f * radius);
    auto capsule = GenerateCapsule(radius, cyl, 12);
    const DeferredMaterial mat =
        matlib_.SolidColor(glm::vec3(c.color_r, c.color_g, c.color_b), 0.6f);
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f), glm::vec3(c.pos_x, ground, c.pos_z));
    const std::string name = "unit_" + std::to_string(index++);
    unit_nodes_.push_back(AddMeshEntity(scene_, name.c_str(), std::move(capsule), mat, xf));
  }
}

void GameView::HandleEvent(const SDL_Event& event, int /*width*/,
                           int /*height*/) {
  // Fixed-angle camera: only zoom is mouse-driven (wheel + trackpad, which SDL
  // reports as the same event with fractional deltas). Key panning is read
  // directly from Update()'s keyboard_state snapshot instead of per-event.
  //
  // Use the window's LOGICAL size (EventWindowLogicalSize), not HandleEvent's
  // physical-pixel width/height: SDL mouse coords are points, so mixing them
  // with a pixel extent scales the anchor ray off the cursor on HiDPI displays.
  if (event.type != SDL_EVENT_MOUSE_WHEEL) return;
  if (ImGui::GetIO().WantCaptureMouse) return;
  glm::vec2 screen;
  if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
  ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel),
               glm::vec2(event.wheel.mouse_x, event.wheel.mouse_y), screen);
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

  // Fixed-interval game logic: run sim_.Tick(kTickDt) until we've caught up to
  // the clock's tick target. Bounded (real dt is clamped in Advance); the
  // budget is pure spiral-of-death safety. Ticks scale with sim speed because
  // the target is derived from sim time.
  {
    PROFILE_SCOPE("game_ticks");
    const unsigned long long tick_target = sim_clock_.TickTarget();
    int budget = kMaxSimTicksPerFrame;
    while (sim_ticks_done_ < tick_target && budget-- > 0) {
      sim_.Tick(static_cast<float>(kTickDt));
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
  SyncUnits();  // live AI capsules on the terrain, rebuilt from the snapshot
  scene_.SyncToRegistry(registry_, scene_context_);
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

  const auto world = sim_.World();
  sim_.Buildings(building_rows_);

  ImGui::Begin("World");
  ImGui::Text("Gold: %u", world.gold);
  ImGui::Text("Buildings: %u", static_cast<uint32_t>(building_rows_.size()));
  ImGui::Separator();
  for (const BuildingState& b : building_rows_) {
    ImGui::Text("#%u %-24s (%.1f, %.1f)", b.id,
               building_label(b.kind),
               b.center_x, b.center_z);
  }
  ImGui::End();
}

void GameView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
