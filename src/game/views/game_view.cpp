#include "game/views/game_view.hpp"

#include <algorithm>
#include <cmath>
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

// rotation_index -> world yaw is YawFromRotationIndex (game/visual/
// selection_decals.hpp). It lives there rather than here so the building mesh
// and its selection outline cannot drift apart -- see building_scene.h's
// AddBuildingToScene comment for why the mapping is exact for 0/2 and an
// approximation for 1/3.

// Game-UI text size in LOGICAL pixels. The atlas is baked once at this size, so
// changing it means a re-bake, not a re-layout.
constexpr float kHudFontPx = 18.0f;

// Display name for a character: its own name if it has one (heroes), else an
// archetype word. `concrete` picks the flavour: the floating WORLD LABELS want
// the concrete creatures the sim ships ("Rat"/"Deer"), while the detail panel
// reads the generic archetype ("Monster"/"Critter"). One switch, two callers.
std::string character_display_name(const CharacterState& c, bool concrete) {
  if (c.name[0] != '\0') return c.name;
  switch (static_cast<Archetype>(c.archetype)) {
    case Archetype::Hero: return "Hero";
    case Archetype::Townfolk: return "Townfolk";
    case Archetype::Critter: return concrete ? "Deer" : "Critter";
    case Archetype::Monster: return concrete ? "Rat" : "Monster";
  }
  return "Unit";
}

// The building row with `id`, or nullptr. Linear scan over the handful of town
// buildings (the same set picking reads).
const BuildingState* FindBuilding(const std::vector<BuildingState>& rows,
                                  uint32_t id) {
  for (const BuildingState& b : rows) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

// The alive character row with `id`, or nullptr -- including indoors units
// (unlike SelectedUnit), so a visitor chosen from a building's list stays
// inspectable even though it isn't drawn.
const CharacterState* FindCharacter(const std::vector<CharacterState>& rows,
                                    uint32_t id) {
  if (id == kNoPick) return nullptr;
  for (const CharacterState& c : rows) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

// Display class for a picked unit: a hero's guild class, derived from its home
// building's kind (the sim's own guild_hero_class rule -- heroes carry no class
// field), else the archetype so a townfolk/critter/monster reads honestly.
std::string unit_class(const CharacterState& c,
                       const std::vector<BuildingState>& buildings) {
  if (c.home_building_id >= 0) {
    if (const BuildingState* home =
            FindBuilding(buildings, static_cast<uint32_t>(c.home_building_id))) {
      const BuildingDef def = BuildingDefOf(home->kind);
      if (def.recruit_count > 0) return HeroClassName(def.recruits[0]);
    }
  }
  switch (static_cast<Archetype>(c.archetype)) {
    case Archetype::Hero: return "Hero";
    case Archetype::Townfolk: return "Townfolk";
    case Archetype::Critter: return "Critter";
    case Archetype::Monster: return "Monster";
  }
  return "";
}

// --- floating world-label styling (fixed constants; no runtime knobs) -------
constexpr uint32_t kNameColor = 0xEDE6D4FFu;    // warm off-white
constexpr uint32_t kDamageColor = 0xFF5A4BFFu;  // red-orange damage numbers
constexpr uint32_t kHealthBgColor = 0x5A1512E6u;  // dark red (missing health)
constexpr uint32_t kHealthFgColor = 0x53C24CFFu;  // green (current health)
constexpr float kNameLift = 0.6f;          // world units above a unit's head
constexpr float kBuildingNameLift = 4.0f;  // world units above a building base
constexpr float kBarDrop = 0.3f;           // world units below the feet/base
constexpr float kDamageLift = 0.4f;        // world units above the head
constexpr float kDamageLifetime = 1.2f;    // seconds a damage number lives
constexpr float kBarWidth = 34.0f;         // health-bar size at scale 1 (px)
constexpr float kBarHeight = 5.0f;

// Folds an animation opacity into a base 0xRRGGBBAA color's alpha.
uint32_t WithOpacity(uint32_t rgba, float opacity) {
  const float a = static_cast<float>(rgba & 0xffu) * std::clamp(opacity, 0.0f, 1.0f);
  return (rgba & 0xffffff00u) | (static_cast<uint32_t>(a) & 0xffu);
}

// Shapes `text` at a projected anchor and appends its (scaled, centered) glyph
// quads to `out`. `scratch` is a reused ui_text_run buffer. The run comes back
// baseline-left at (0,0); we scale by the anchor's depth scale, center it
// horizontally over the anchor, and put the baseline at the anchor point.
void EmitText(std::vector<UiQuad>& out, std::vector<UiQuad>& scratch,
              UiContext* ctx, const LabelProjection& p, const std::string& text,
              uint32_t color) {
  if (text.empty()) return;
  // Inked glyph quads <= chars <= bytes, so text.size() is a safe upper bound on
  // the count: shape ONCE into a buffer that big (never truncated, count <= cap
  // => UI_OK) rather than a measure pass followed by a fill pass.
  scratch.resize(text.size());
  uint32_t count = 0;
  float w = 0.0f, h = 0.0f;
  if (ui_text_run(ctx, text.data(), static_cast<uint32_t>(text.size()), color,
                  scratch.data(), static_cast<uint32_t>(scratch.size()), &count,
                  &w, &h) != UI_OK) {
    return;
  }
  const float s = p.scale;
  const float ox = p.x - w * s * 0.5f;  // center horizontally over the anchor
  const float oy = p.y;                 // baseline sits at the projected anchor
  for (uint32_t i = 0; i < count; ++i) {  // scratch is oversized; only [0,count)
    UiQuad q = scratch[i];
    q.x = ox + q.x * s;
    q.y = oy + q.y * s;
    q.w *= s;
    q.h *= s;
    out.push_back(q);
  }
}

// Appends a two-quad health bar (red background + green fill) centered at the
// projected anchor, sized by the anchor's depth scale.
void EmitBar(std::vector<UiQuad>& out, const UiFontInfo& font,
             const LabelProjection& p, float frac) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  const float s = p.scale;
  const float bw = kBarWidth * s;
  const float bh = kBarHeight * s;
  const float x = p.x - bw * 0.5f;
  const float y = p.y;
  auto solid = [&](float qx, float qy, float qw, float qh, uint32_t rgba) {
    if (qw <= 0.0f) return;
    UiQuad q{};
    q.x = qx;
    q.y = qy;
    q.w = qw;
    q.h = qh;
    q.u0 = font.white_u;
    q.v0 = font.white_v;
    q.u1 = font.white_u;
    q.v1 = font.white_v;
    q.rgba = rgba;
    out.push_back(q);
  };
  solid(x, y, bw, bh, kHealthBgColor);         // full-width red backing
  solid(x, y, bw * frac, bh, kHealthFgColor);  // green current-health fill
}

// Day/night: the sky cube + SH + IBL prefilter are re-baked at most once per
// this much REAL time (the directional light + shadows still move every frame).
// A real-time throttle bounds the ~per-frame HW cube bake cost independent of
// framerate and sim speed; an Update(0) never re-bakes. Seeks/config edits
// force an immediate re-bake.
constexpr double kRebakeIntervalSeconds = 1.0 / 20.0;  // ~20 Hz sky/IBL refresh

// Day/night cadence: a full 24 in-game-hour cycle takes this many real-world
// MINUTES at 1x sim speed. This is the single knob for how fast the sun moves.
//
// The day/night calendar and the physics sim run on independent rates off the
// same clock (SimClock::sim_seconds): the calendar divides by
// real_seconds_per_day (fast -- 24 h in 3 min), while physics divides by
// kTickDt so it stays 1 in-game second == 1 real second at 1x (kSimHz ticks per
// sim-second). Both scale together with sim speed.
constexpr float kRealMinutesPerGameDay = 3.0f;
constexpr float kRealSecondsPerDay = kRealMinutesPerGameDay * 60.0f;  // 180 s

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

// Sim and render coordinates are IDENTICAL: SeedTown places the town on the
// real plains and BuildScene/SyncUnits render everything at its true sim
// position (the old +z demo-building shift is gone). So the fog-of-war overlay's
// sim->render offset is zero.

// Fog-of-war vision grid (SIM frame). Sized to the WHOLE map so the only hard
// black edge is the real map boundary -- everything inside is organic FoW
// (discovered vs not). (It was 64 = half the map, a stale value from when the
// gameplay grid was 96 m; anything past 64 rendered permanently black, cutting
// the southern town. See kGridHalfExtentTiles, now the full map.)
constexpr float kVisionHalfExtentM =
    static_cast<float>(kGridHalfExtentTiles);  // 128 = map half-extent
constexpr float kVisionTexelM = 1.0f;

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

  // sim_ is constructed with a nullptr brain_script_source (mock brains only;
  // no noiser script needed for a static-buildings scaffold) -- construction
  // also prebuilds the Castle at kCastleSpawn (the plains, at the town centre).
  SeedTown();
  BuildScene();
  // Water test map: keep the frame clear (no volumetric fog obscuring the lake).
  if (scene_renderer_) scene_renderer_->MutableFogConfig().enabled = false;

  // Fog-of-war overlay. Configure the SIM-frame vision grid, register the pass
  // on scene_context_ (picked up by any renderer, incl. the headless
  // --screenshot one), and resolve once now (ResolveVision publishes without
  // advancing the sim) so the first frame has a populated field. sim == render,
  // so the sim->render offset is zero.
  if (!vision_pass_.Initialize(device_, queue_, ctx.pipeline_gen)) {
    spdlog::error("GameView::Initialize: vision overlay pass init failed");
    return false;
  }
  vision_pass_.SetSimToWorldOffset(glm::vec2(0.0f, 0.0f));
  sim_.ConfigureVision(-kVisionHalfExtentM, -kVisionHalfExtentM,
                       2.0f * kVisionHalfExtentM, 2.0f * kVisionHalfExtentM,
                       kVisionTexelM);
  sim_.ResolveVision();
  vision_pass_.Upload(sim_.GetVisionField());

  // Vision-cone debug overlay (off by default; toggled in the debug panel).
  // Chained AFTER the fog-of-war overlay so the cones draw on top of the
  // FoW-modulated scene rather than being blacked out by it.
  if (!cone_pass_.Initialize(device_, queue_, ctx.pipeline_gen)) {
    spdlog::error("GameView::Initialize: cone overlay pass init failed");
    return false;
  }
  post_passes_.Add(&vision_pass_);
  post_passes_.Add(&cone_pass_);
  scene_context_.post_pass = &post_passes_;

  // Frame the living town on the southern plains, centred on the colony Castle
  // (kCastleSpawn) rather than the empty origin lake -- close enough that the
  // units read as characters. Scroll to zoom out to the whole 256 m map; WASD to
  // roam.
  gamecam_.focus = glm::vec3(kCastleSpawnX, 0.0f, kCastleSpawnZ);
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
  // so buildings render where the sim places them, no shift. The prebuilt Castle
  // (kCastleSpawn, id 0) sits at the town centre and is the colony seat + the
  // tax collector's deposit point; these buildings ring it. The central lake at
  // the map origin is now just scenery.
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
                          YawFromRotationIndex(b.rotation_index), ground);
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

  // character_rows_ is the frame's single character snapshot, taken by
  // SnapshotCharacters() earlier in Update() and shared with RefreshHud +
  // world picking -- read it, don't re-snapshot.
  int index = 0;
  for (const CharacterState& c : character_rows_) {
    if (c.inside_building_id >= 0) {
      continue;  // hidden
    }
    const float ground = GroundAt(c.pos_x, c.pos_z);
    const float radius = 0.5f * std::min(c.size_x, c.size_z);
    const float cyl = std::max(0.1f, c.size_y - 2.0f * radius);
    const glm::vec3 color(c.color_r, c.color_g, c.color_b);
    const DeferredMaterial mat = matlib_.SolidColor(color, 0.6f);

    // Face the direction of travel: kCharacterForward is +Z, so yaw = atan2 of
    // the facing XZ turns +Z onto (facing_x, facing_z). Applied to the body (a
    // symmetric capsule, so invisible on its own) AND to a small nose that makes
    // the facing legible.
    const float yaw = std::atan2(c.facing_x, c.facing_z);
    const glm::mat4 base =
        glm::translate(glm::mat4(1.0f), glm::vec3(c.pos_x, ground, c.pos_z)) *
        glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));

    auto capsule = GenerateCapsule(radius, cyl, 12);
    const std::string body = "unit_" + std::to_string(index) + "_body";
    unit_nodes_.push_back(AddMeshEntity(scene_, body.c_str(), std::move(capsule), mat, base));

    // A small nose nub at the front (local +Z), mid-body height, so which way a
    // unit faces reads at a glance even though the capsule is round.
    const float nose = 0.35f * radius;
    auto nub = GenerateCube(glm::vec3(nose));
    const glm::mat4 nose_xf =
        base * glm::translate(glm::mat4(1.0f),
                              glm::vec3(0.0f, 0.5f * c.size_y, radius + nose));
    const std::string nose_name = "unit_" + std::to_string(index) + "_nose";
    unit_nodes_.push_back(
        AddMeshEntity(scene_, nose_name.c_str(), std::move(nub), mat, nose_xf));
    ++index;
  }

  // Vision-cone debug overlay geometry (only when the toggle is on): a flat,
  // translucent sector per vision-granting unit, floating just above terrain and
  // oriented by facing. Fed to the cone post-pass; empty clears it.
  if (cone_pass_.enabled()) {
    cone_pass_.SetTriangles(BuildVisionConeTriangles());
  } else {
    cone_pass_.SetTriangles({});
  }
}

float GameView::GroundAt(float world_x, float world_z) const {
  return map_.empty() ? 0.0f
                      : map_.HeightAt(world_x + half_x_, world_z + half_z_);
}

void GameView::RefreshSelectionDecals() {
  decals_.clear();

  // A unit that is inside a building is not drawn, so it gets no highlight even
  // though the panel can still describe it (a visitor picked from a building's
  // list) -- a ring on empty ground would be a lie.
  if (const CharacterState* c = FindCharacter(character_rows_, selected_hero_)) {
    if (c->inside_building_id < 0) {
      decals_.push_back(MakeUnitRing(*c, GroundAt(c->pos_x, c->pos_z)));
    }
  }
  if (const BuildingState* b = FindBuilding(building_rows_, selected_building_)) {
    decals_.push_back(MakeBuildingRect(*b, GroundAt(b->center_x, b->center_z)));
  }

  // Republished every frame: decals_ may have reallocated, and an empty list
  // must clear the previous frame's pointer rather than leave it dangling.
  scene_context_.decals = decals_.empty() ? nullptr : decals_.data();
  scene_context_.decal_count = static_cast<uint32_t>(decals_.size());
}

std::vector<float> GameView::BuildVisionConeTriangles() const {
  constexpr float kConeLift = 0.4f;   // metres above terrain, so it clearly floats
  constexpr float kConeAlpha = 0.28f;  // straight-alpha translucency
  constexpr float kSegRad = 0.1745f;   // ~10 deg per fan segment
  std::vector<float> v;
  for (const CharacterState& c : character_rows_) {
    if (c.vision_radius <= 0.0f || c.inside_building_id >= 0) {
      continue;  // no vision, or hidden inside a building
    }
    const float y = GroundAt(c.pos_x, c.pos_z) + kConeLift;
    const float r = c.vision_radius;
    const float center = std::atan2(c.facing_x, c.facing_z);  // +Z is 0
    const float half = glm::radians(std::min(c.vision_cone_half_angle_deg, 180.0f));
    const int segs = std::max(3, static_cast<int>(std::ceil(2.0f * half / kSegRad)));
    auto push = [&](float x, float z) {
      v.insert(v.end(), {x, y, z, c.color_r, c.color_g, c.color_b, kConeAlpha});
    };
    for (int i = 0; i < segs; ++i) {
      const float a0 = center - half + 2.0f * half * static_cast<float>(i) / segs;
      const float a1 = center - half + 2.0f * half * static_cast<float>(i + 1) / segs;
      push(c.pos_x, c.pos_z);                                              // apex
      push(c.pos_x + std::sin(a0) * r, c.pos_z + std::cos(a0) * r);       // arc i
      push(c.pos_x + std::sin(a1) * r, c.pos_z + std::cos(a1) * r);       // arc i+1
    }
  }
  return v;
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
      // Over the combat log? Scroll it rather than zooming the camera. The HUD
      // hit rects are physical pixels, so convert the logical cursor first (same
      // conversion the click path uses).
      const glm::vec2 to_physical(
          screen.x > 0.0f ? static_cast<float>(width) / screen.x : 1.0f,
          screen.y > 0.0f ? static_cast<float>(height) / screen.y : 1.0f);
      const glm::vec2 cur_physical(event.wheel.mouse_x * to_physical.x,
                                   event.wheel.mouse_y * to_physical.y);
      if (HudHitTest(hud_frame_, cur_physical.x, cur_physical.y) == kHudCombatLog) {
        ScrollCombatLog(NormalizedWheelY(event.wheel));
        return;
      }
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

      // World pick: ray against the terrain surface, then the drawn footprints.
      // Sim and render coordinates are identical now (the old demo south-shift is
      // gone), so the picked XZ is already sim space. GroundPickXZ follows the
      // terrain height (not the y=0 plane) so a unit seated on elevated terrain
      // is picked where it appears rather than parallaxed off its footprint.
      const Ray ray = ScreenPointToRay(camera_, cursor_logical, logical);
      glm::vec2 sim_world;
      if (!GroundPickXZ(ray.origin, ray.dir,
                        [this](glm::vec2 p) {
                          return map_.empty()
                                     ? 0.0f
                                     : map_.HeightAt(p.x + half_x_, p.y + half_z_);
                        },
                        sim_world)) {
        return;  // at/above the horizon
      }

      // Units (small foreground capsules) win over the larger building footprints
      // they may stand on. character_rows_ / building_rows_ are this frame's
      // snapshot, so a pick hits exactly what was last drawn; HeroAtWorld skips
      // hidden (inside-building) units, which aren't drawn.
      selected_hero_ = HeroAtWorld(
          character_rows_.data(),
          static_cast<uint32_t>(character_rows_.size()), sim_world);
      // A world pick never selects an indoors unit (HeroAtWorld skips them), so
      // this selection follows the drawn-only rule.
      selected_unit_from_list_ = false;
      selected_building_ =
          selected_hero_ != kNoPick
              ? kNoPick
              : BuildingAtWorld(
                    building_rows_.data(),
                    static_cast<uint32_t>(building_rows_.size()), sim_world);
      return;
    }

    case SDL_EVENT_KEY_DOWN:
      if (io.WantCaptureKeyboard) return;
      if (event.key.key == SDLK_ESCAPE) {
        selected_building_ = kNoPick;
        selected_hero_ = kNoPick;
        selected_unit_from_list_ = false;
      }
      return;

    default:
      return;
  }
}

uint32_t GameView::AddSelectTarget(HudSelectTarget::Kind kind, uint32_t id) {
  const uint32_t handle =
      kHudSelectBase + static_cast<uint32_t>(hud_targets_.size());
  hud_targets_.push_back({kind, id});
  return handle;
}

bool GameView::DispatchHudAction(uint32_t hud_id) {
  // Sim-speed buttons: a presentation-clock op (SimClock::speed), not a sim
  // action -- pausing/scaling time changes how many ticks run per frame.
  switch (hud_id) {
    case kHudBtnPause: sim_clock_.speed = 0.0f; return true;
    case kHudBtnSpeed1: sim_clock_.speed = 1.0f; return true;
    case kHudBtnSpeed2: sim_clock_.speed = 2.0f; return true;
    case kHudBtnSpeed4: sim_clock_.speed = 4.0f; return true;
    default: break;
  }

  // Clickable entity rows (residents / visitors / a hero's home link) change the
  // selection rather than firing an action. hud_targets_ was rebuilt this frame
  // by RefreshHud in lockstep with the ids it tagged those rows with.
  if (hud_id >= kHudSelectBase) {
    const uint32_t idx = hud_id - kHudSelectBase;
    if (idx >= hud_targets_.size()) return false;
    const HudSelectTarget& t = hud_targets_[idx];
    if (t.kind == HudSelectTarget::Kind::Building) {
      selected_building_ = t.id;
      selected_hero_ = kNoPick;
      selected_unit_from_list_ = false;
    } else {
      selected_hero_ = t.id;
      selected_building_ = kNoPick;
      selected_unit_from_list_ = true;  // a visitor may be indoors but inspectable
    }
    return true;
  }

  Action action{};
  switch (hud_id) {
    case kHudBtnRecruit:
      if (selected_building_ == kNoPick) return false;
      action.kind = ActionKind::RecruitHero;
      action.target_id = selected_building_;
      break;
    case kHudBtnDestroy:
      if (selected_building_ == kNoPick) return false;
      action.kind = ActionKind::DestroyBuilding;
      action.target_id = selected_building_;
      break;
    default:
      return false;  // panel/bar background: consumed, but not an action
  }
  const int64_t rc = sim_.Dispatch(action);
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

std::string GameView::EventActorName(uint32_t slot) const {
  // Current frame's names first, then last frame's (a just-dead entity), else "?".
  auto it = char_names_.find(slot);
  if (it != char_names_.end()) return it->second;
  auto pit = char_names_prev_.find(slot);
  return pit != char_names_prev_.end() ? pit->second : "?";
}

float GameView::EventActorSize(uint32_t slot) const {
  auto it = char_sizes_.find(slot);
  if (it != char_sizes_.end()) return it->second;
  auto pit = char_sizes_prev_.find(slot);
  return pit != char_sizes_prev_.end() ? pit->second : 1.2f;  // never-seen fallback
}

std::string GameView::EventTargetName(const GameEvent& e) const {
  if (e.target_kind == kEventTargetBuilding) {
    auto it = building_kinds_.find(e.target_id);
    if (it != building_kinds_.end()) return building_label(it->second);
    auto pit = building_kinds_prev_.find(e.target_id);
    return pit != building_kinds_prev_.end()
               ? std::string(building_label(pit->second))
               : "Building";
  }
  return EventActorName(e.target_id);  // same two-buffer character-name lookup
}

void GameView::PushLogLine(std::string line) {
  constexpr size_t kLogCap = 200;
  combat_log_lines_.push_back(std::move(line));
  if (combat_log_lines_.size() > kLogCap) {
    combat_log_lines_.erase(
        combat_log_lines_.begin(),
        combat_log_lines_.begin() +
            static_cast<long>(combat_log_lines_.size() - kLogCap));
  }
  // If the user is reading older lines, keep their window on the same content as
  // new lines arrive (RefreshHud clamps the offset to the current total).
  if (combat_log_scroll_ > 0) ++combat_log_scroll_;
}

void GameView::ScrollCombatLog(float wheel_y) {
  // Wheel up (positive) scrolls back into older lines; down returns to newest.
  if (wheel_y > 0.0f) {
    ++combat_log_scroll_;
  } else if (wheel_y < 0.0f && combat_log_scroll_ > 0) {
    --combat_log_scroll_;
  }
}

void GameView::PumpGameEvents() {
  sim_.DrainEvents(events_scratch_);
  if (events_scratch_.empty()) {
    return;  // No events this frame -> skip the name-cache rebuild entirely.
  }

  // Double-buffer the name/size caches: last frame's set moves to _prev_, this
  // frame's is rebuilt from the live snapshot. An entity downed/destroyed this
  // frame is gone from the snapshot but still in _prev_ (alive last event frame),
  // so its log line + damage number still resolve. Rebuilt only on frames that
  // actually have events, so quiet frames pay nothing.
  char_names_prev_.swap(char_names_);
  char_names_.clear();
  char_sizes_prev_.swap(char_sizes_);
  char_sizes_.clear();
  for (const CharacterState& c : character_rows_) {
    char_names_[c.id] = character_display_name(c, /*concrete=*/true);
    char_sizes_[c.id] = c.size_y;
  }
  building_kinds_prev_.swap(building_kinds_);
  building_kinds_.clear();
  for (const BuildingState& b : building_rows_) {
    building_kinds_[b.id] = b.kind;
  }

  for (const GameEvent& e : events_scratch_) {
    switch (e.kind) {
      case GameEventKind::DamageDealt: {
        PushLogLine(EventActorName(e.actor_id) + " -> " + EventTargetName(e) +
                    "  " + std::to_string(static_cast<int>(e.amount)));
        // Floating damage numbers are for CHARACTERS (per the design); building
        // damage goes to the log only. Anchor to the live victim when it still
        // exists (the number follows a moving unit), else the event position
        // (a lethal hit's number floats where the victim died).
        if (e.target_kind == kEventTargetCharacter) {
          const CharacterState* v = FindCharacter(character_rows_, e.target_id);
          const glm::vec3 pos =
              v ? glm::vec3(v->pos_x, GroundAt(v->pos_x, v->pos_z), v->pos_z)
                : glm::vec3(e.x, GroundAt(e.x, e.z), e.z);
          const float head = (v ? v->size_y : EventActorSize(e.target_id)) + kDamageLift;
          timed_labels_.Spawn(e.target_id, pos, head,
                              std::to_string(static_cast<int>(e.amount)),
                              kDamageColor, kDamageLifetime,
                              LabelAnimation::RiseFade);
        }
        break;
      }
      case GameEventKind::HeroDowned:
        PushLogLine(EventTargetName(e) + " downed");
        break;
      case GameEventKind::BuildingDestroyed:
        PushLogLine(EventTargetName(e) + " destroyed");
        break;
      case GameEventKind::HeroDied:
        break;  // reserved: not emitted yet
    }
  }
}

void GameView::BuildFloatingLabels(std::vector<UiQuad>& out) {
  out.clear();
  // Only the baked font context is needed to SHAPE quads (ui_text_run + the
  // white texel); the pipeline/variant is ensured later by Prepare/Draw. Gating
  // on the context (not full ready()) keeps labels from dropping the first frame.
  if (!ui_ || ui_->context() == nullptr || ui_viewport_w_ <= 0.0f) return;
  UiContext* ctx = ui_->context();
  const UiFontInfo& font = ui_->font_info();
  const glm::mat4 vp = camera_.GetProj() * camera_.GetView();

  // Character names (above the head) + health bars (below the feet).
  for (const CharacterState& c : character_rows_) {
    if (c.inside_building_id >= 0) continue;  // hidden indoors -> no label
    const float ground = GroundAt(c.pos_x, c.pos_z);
    const LabelProjection np = ProjectLabel(
        vp, {c.pos_x, ground + c.size_y + kNameLift, c.pos_z}, ui_viewport_w_,
        ui_viewport_h_);
    if (np.visible) {
      EmitText(out, label_run_scratch_, ctx, np,
               character_display_name(c, /*concrete=*/true), kNameColor);
    }
    if (c.max_hp > 0.0f) {
      const LabelProjection bp = ProjectLabel(
          vp, {c.pos_x, ground - kBarDrop, c.pos_z}, ui_viewport_w_, ui_viewport_h_);
      if (bp.visible) EmitBar(out, font, bp, c.hp / c.max_hp);
    }
  }

  // Building names (above the base) + health bars (below the base).
  for (const BuildingState& b : building_rows_) {
    const float ground = GroundAt(b.center_x, b.center_z);
    const LabelProjection np = ProjectLabel(
        vp, {b.center_x, ground + kBuildingNameLift, b.center_z}, ui_viewport_w_,
        ui_viewport_h_);
    if (np.visible) {
      EmitText(out, label_run_scratch_, ctx, np, building_label(b.kind), kNameColor);
    }
    if (b.max_hp > 0.0f) {
      const LabelProjection bp = ProjectLabel(
          vp, {b.center_x, ground - kBarDrop, b.center_z}, ui_viewport_w_,
          ui_viewport_h_);
      if (bp.visible) EmitBar(out, font, bp, b.hp / b.max_hp);
    }
  }

  // Floating damage numbers: resolve the timed pool, following live victims.
  const AnchorLookup live = [this](uint32_t slot) -> std::optional<glm::vec3> {
    const CharacterState* c = FindCharacter(character_rows_, slot);
    if (!c) return std::nullopt;
    return glm::vec3(c->pos_x, GroundAt(c->pos_x, c->pos_z), c->pos_z);
  };
  for (const ResolvedLabel& l : timed_labels_.Resolve(live)) {
    const LabelProjection p =
        ProjectLabel(vp, l.world_pos, ui_viewport_w_, ui_viewport_h_);
    if (p.visible) {
      EmitText(out, label_run_scratch_, ctx, p, l.text,
               WithOpacity(l.color, l.opacity));
    }
  }
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

  // The UI-domain clock: raw dt, unscaled by game speed and unaffected by
  // pause, so selection-decal dashes keep marching on a paused world.
  real_time_seconds_ += real_dt;
  scene_context_.real_time_seconds = static_cast<float>(real_time_seconds_);

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
  bool ticked_this_frame = false;
  {
    PROFILE_SCOPE("game_ticks");
    const unsigned long long tick_target = sim_clock_.TickTarget();
    int budget = kMaxSimTicksPerFrame;
    while (sim_ticks_done_ < tick_target && budget-- > 0) {
      sim_.Tick(static_cast<float>(kTickDt));
      ++sim_ticks_done_;
      ticked_this_frame = true;
    }
  }

  // Age the floating damage numbers on the REAL clock (independent of sim
  // speed/pause), so they fade smoothly regardless of tick rate. New numbers are
  // spawned in RefreshHud (PumpGameEvents) at full lifetime, after this ages the
  // existing ones.
  timed_labels_.Advance(static_cast<float>(real_dt));

  // Fog-of-war: re-upload the visibility field to the overlay texture only when
  // a sim tick actually ran this frame (the field is unchanged otherwise, and
  // render runs faster than the fixed sim tick). The initial field is uploaded
  // in Initialize.
  if (ticked_this_frame) {
    vision_pass_.Upload(sim_.GetVisionField());
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
  SnapshotCharacters();  // one snapshot; SyncUnits + RefreshHud + picking share it
  SyncUnits();  // live AI capsules on the terrain, rebuilt from the snapshot
  scene_.SyncToRegistry(registry_, scene_context_);

  {
    PROFILE_SCOPE("hud");
    RefreshHud();
  }

  // After RefreshHud: that is where a selection pointing at something that no
  // longer exists gets dropped, so the decals follow a validated selection.
  RefreshSelectionDecals();
}

uint32_t GameView::SnapshotBuildings() {
  // sim_.Buildings sizes building_rows_ to the LIVE count (never a padded
  // capacity): picking reads building_rows_.size(), so a stale tail would let a
  // click land on a dead row (e.g. after a Destroy, before the next refresh).
  sim_.Buildings(building_rows_);
  return static_cast<uint32_t>(building_rows_.size());
}

uint32_t GameView::SnapshotCharacters() {
  // Sized to the LIVE character count (same contract as SnapshotBuildings), and
  // reuses the buffer's capacity via the fill overload. Taken once in Update()
  // before every consumer, so there is no ordering dependency between them.
  sim_.Characters(character_rows_);
  return static_cast<uint32_t>(character_rows_.size());
}

void GameView::RefreshHud() {
  // Snapshot the buildings into the reused buffer; character_rows_ was already
  // filled by SyncUnits() earlier this frame. Both back the HUD model AND
  // picking, so what the panel describes is exactly what a click can hit.
  // Update() runs this before DrawUI() the same frame, so DrawUI reuses these
  // rows + the cached scalars below instead of re-reading them from the sim.
  hud_building_total_ = SnapshotBuildings();

  // Drain this frame's sim events (both snapshots are now current): appends
  // combat-log lines and spawns floating damage numbers. Runs before the early
  // return below so the event buffer drains even without a UI context.
  PumpGameEvents();

  const WorldState world = sim_.World();
  roster_cap_ = world.guild_roster_cap;
  hud_gold_ = world.gold;

  HudModel model;
  model.gold = world.gold;
  model.speed = sim_clock_.speed;
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

  // A selection that no longer exists (destroyed, or a unit that entered a
  // building) silently clears rather than describing a stale/undrawn row.
  // SelectedUnit enforces the same "indoors units aren't selectable" rule as
  // the pick, so a unit that walks inside drops the panel.
  const BuildingState* selected =
      FindBuilding(building_rows_, selected_building_);
  // A world pick never selects an indoors unit, so SelectedUnit's "drop indoors"
  // rule keeps the panel honest. A list pick (a building's visitor) is meant to
  // stay inspectable even indoors, so resolve it by id including hidden units.
  const CharacterState* hero =
      selected_unit_from_list_
          ? FindCharacter(character_rows_, selected_hero_)
          : SelectedUnit(character_rows_.data(),
                         static_cast<uint32_t>(character_rows_.size()),
                         selected_hero_);
  if (!selected) selected_building_ = kNoPick;
  if (!hero) selected_hero_ = kNoPick;

  // Nothing to render into without a live UI context. Return BEFORE touching
  // hud_targets_, so it stays paired with the last hud_frame_ we actually built
  // -- a click resolves a hit-rect id back through the SAME frame's target
  // table. (The per-frame work above -- snapshots, cached scalars, selection
  // validity -- must still run every frame for picking and the debug windows.)
  if (!ui_ || ui_viewport_w_ <= 0.0f) return;
  hud_targets_.clear();  // rebuilt below, in lockstep with the clickable rows

  if (selected) {
    const BuildingDef def = BuildingDefOf(selected->kind);

    HudSelection s;
    s.kind = HudSelection::Kind::Building;
    s.id = selected->id;
    s.title = building_label(selected->kind);
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
    if (selected->max_hp > 0.0f) {
      char hp[48];
      std::snprintf(hp, sizeof(hp), "%.0f / %.0f", selected->hp,
                    selected->max_hp);
      s.rows.emplace_back("health", hp);
    }
    // Uncollected tax owed (Houses accrue at midnight; a collector zeroes it).
    if (selected->taxable_income > 0) {
      s.rows.emplace_back("taxable",
                          std::to_string(selected->taxable_income) + " g");
    }
    // A guild is a building that recruits >= 1 hero class; only those show the
    // recruited-class row and the Recruit button. Gating on def.recruit_count --
    // read straight off BuildingDef::recruits, the sim's own per-kind recruit
    // table -- means the button is never shown enabled for a building
    // Dispatch(RecruitHero) would reject.
    const bool is_guild = def.recruit_count > 0;
    if (is_guild) {
      std::string classes;
      for (int32_t i = 0; i < def.recruit_count && i < kMaxRecruitClasses; ++i) {
        if (i > 0) classes += ", ";
        classes += HeroClassName(def.recruits[i]);
      }
      s.rows.emplace_back("recruits", classes);
      s.show_recruit = true;
    }

    // Builds a clickable unit list from the characters matching `match`, capped
    // at kListCap with a "+N more" overflow tail (the ui crate can't scroll
    // yet); `value(c)` fills the right-hand column, and each row selects that
    // unit. Returns the TOTAL matched (may exceed the shown entries) -- for
    // residents that is the roster occupancy. Roster cap is 4 today, so the cap
    // only ever bites for visitors in a crowded building.
    constexpr uint32_t kListCap = 8;
    auto build_unit_list = [&](auto&& match, auto&& value,
                               HudList& out) -> uint32_t {
      uint32_t total = 0;
      for (const CharacterState& c : character_rows_) {
        if (!match(c)) continue;
        ++total;
        if (out.entries.size() < kListCap) {
          out.entries.emplace_back(
              character_display_name(c, /*concrete=*/false), value(c),
              AddSelectTarget(HudSelectTarget::Kind::Hero, c.id));
        }
      }
      out.overflow = total - static_cast<uint32_t>(out.entries.size());
      return total;
    };

    // Residents: heroes whose home guild is this building. A guild always shows
    // the roster count -- even at 0, so its capacity is visible before the first
    // recruit; a non-guild has no roster.
    HudList residents;
    const uint32_t occupancy = build_unit_list(
        [&](const CharacterState& c) {
          return c.home_building_id == static_cast<int32_t>(selected->id);
        },
        [](const CharacterState& c) {
          char hp[24];
          std::snprintf(hp, sizeof(hp), "%.0f/%.0f", c.hp, c.max_hp);
          return std::string(hp);
        },
        residents);
    if (is_guild) {
      residents.heading = "Residents (" + std::to_string(occupancy) + "/" +
                          std::to_string(roster_cap_) + ")";
      s.lists.push_back(std::move(residents));
      s.can_recruit = occupancy < roster_cap_;
    }

    // Visiting: units currently inside this building (hidden, so reachable only
    // through this list).
    HudList visiting;
    const uint32_t visitors = build_unit_list(
        [&](const CharacterState& c) {
          return c.inside_building_id == static_cast<int32_t>(selected->id);
        },
        [&](const CharacterState& c) { return unit_class(c, building_rows_); },
        visiting);
    if (visitors > 0) {
      visiting.heading = "Visiting (" + std::to_string(visitors) + ")";
      s.lists.push_back(std::move(visiting));
    }

    s.show_destroy = def.user_destructible;
    s.can_destroy = s.show_destroy;

    model.has_selection = true;
    model.selection = std::move(s);
  } else if (hero) {
    HudSelection s;
    s.kind = HudSelection::Kind::Hero;
    s.id = hero->id;
    s.title = character_display_name(*hero, /*concrete=*/false);
    s.rows.emplace_back("id", "#" + std::to_string(hero->id));
    {
      // Class: derived from the home guild (heroes carry no class field), else
      // the archetype so a picked townfolk/critter/monster reads honestly.
      const std::string cls = unit_class(*hero, building_rows_);
      if (!cls.empty()) s.rows.emplace_back("class", cls);
    }
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
    // Home guild: a navigable link back to the recruiting building.
    if (hero->home_building_id >= 0) {
      const BuildingState* home = FindBuilding(
          building_rows_, static_cast<uint32_t>(hero->home_building_id));
      const std::string label =
          home ? building_label(home->kind)
               : ("#" + std::to_string(hero->home_building_id));
      const uint32_t click =
          home ? AddSelectTarget(HudSelectTarget::Kind::Building, home->id) : 0;
      s.rows.emplace_back("guild", label, click);
    }
    model.has_selection = true;
    model.selection = std::move(s);
  }

  // Combat log: window the ring buffer to what fits the bottom panel, honouring
  // the scroll offset (0 = following the newest lines at the bottom). Same
  // capacity math the panel lays out with, so they never disagree.
  {
    const uint32_t cap = HudCombatLogCapacity();
    const int total = static_cast<int>(combat_log_lines_.size());
    const int max_scroll = std::max(0, total - static_cast<int>(cap));
    combat_log_scroll_ = std::clamp(combat_log_scroll_, 0, max_scroll);
    const int end = total - combat_log_scroll_;
    const int begin = std::max(0, end - static_cast<int>(cap));
    for (int i = begin; i < end; ++i) {
      model.combat_log.push_back(combat_log_lines_[static_cast<size_t>(i)]);
    }
  }

  BuildHud(ui_->context(), model, ui_viewport_w_, ui_viewport_h_, ui_scale_,
           hud_frame_);

  // Floating world labels draw UNDER the HUD (the HUD/panel must stay on top),
  // so concatenate the world-label quads FIRST, then the HUD quads, into one
  // SetQuads (the UI pass paints in list order).
  BuildFloatingLabels(floating_quads_);
  ui_quads_.clear();
  ui_quads_.insert(ui_quads_.end(), floating_quads_.begin(), floating_quads_.end());
  ui_quads_.insert(ui_quads_.end(), hud_frame_.quads.begin(), hud_frame_.quads.end());
  ui_->SetQuads(ui_quads_.data(), static_cast<uint32_t>(ui_quads_.size()));
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

  // --- Fog-of-war (vision overlay) ---
  if (ImGui::CollapsingHeader("Fog of War", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Enabled##fow", &vision_pass_.mutable_enabled());
    ImGui::TextUnformatted("black = terra-incognita, gray = dormant");
  }

  // --- Gameplay debug layer ---
  if (ImGui::CollapsingHeader("Gameplay Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Vision cones", &cone_pass_.mutable_enabled());
    ImGui::TextUnformatted(
        "translucent sector per unit, above terrain, along facing");
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

  // Reuse this frame's snapshot: Update()->RefreshHud() already read gold and
  // filled building_rows_ (sized to the live count) just before DrawUI runs,
  // so there is no need to round-trip the sim again here.
  ImGui::Begin("World");
  ImGui::Text("Gold: %u", hud_gold_);
  ImGui::Text("Buildings: %u", hud_building_total_);
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
