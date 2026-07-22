#include "ai_sandbox/ai_sandbox_view.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/fixed_timestep.hpp"          // kTickDt
#include "engine/app/game_camera_controller.hpp"  // ZoomAtCursor
#include "engine/app/sdl_input_util.hpp"
#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/building_catalog.h"
#include "game/scene/blockout_materials.hpp"

namespace badlands {

namespace {

// Debug materials come from the shared blockout palette (game/scene/
// blockout_materials.hpp) so the game's blockout mode and this arena draw from
// one source of truth: blockout::kArenaGray/kArenaRoughness for the floor +
// walls, blockout::kWall/kRoof for the buildings, blockout::kCapsule* for the
// hero capsules.

// Wall block footprint/height (world units; tile = 1.0 world unit).
constexpr float kWallHalfFootprint = 0.5f;
constexpr float kWallHalfHeight = 0.6f;  // 1.2 tall

// Repeat the floor UVs roughly once per 2 world units instead of stretching
// one copy across the whole floor.
constexpr float kFloorUvRepeatSpacing = 2.0f;

// Capsule dimensions (world units).
constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleCylinderHeight = 0.6f;

// The sandbox arena: big enough for a small town (the default 13x7 arena is a
// duel pit). Still well inside the sim's 96x96 placement grid.
constexpr glm::ivec2 kSandboxArena{44, 30};

// Snapshot buffer caps. The sandbox town is tiny; a truncated snapshot would
// only mean fewer rows drawn/listed, never a crash.
constexpr uint32_t kMaxCharacterRows = 64;
constexpr uint32_t kMaxBuildingRows = 64;
constexpr uint32_t kMaxCommandRows = 24;

// Where the seeded town goes. Kept clear of the origin Castle that game_create
// prebuilds (4x4 + margin) and spaced so no footprint+margin overlaps another.
struct TownBuilding {
  GameBuildingKind kind;
  float x, z;
  int32_t rotation_index;
};
constexpr TownBuilding kTown[] = {
    {GAME_BUILDING_FREE_COMPANY_QUARTERS, -14.0f, -8.0f, 0},
    {GAME_BUILDING_FREE_COMPANY_QUARTERS, -14.0f, 8.0f, 0},
    {GAME_BUILDING_TAVERN, 14.0f, -8.0f, 0},
    {GAME_BUILDING_APOTHECARY, 14.0f, 8.0f, 0},
};

// Heroes recruited per guild at seed time. Below kGuildRosterCap so the panel
// can show a recruit that fails on a full roster if the cap ever shrinks.
constexpr int kSeedHeroesPerGuild = 3;

// Human-readable badlands::Behavior (game/src/town_brain.h). The sandbox is a
// consumer of the ABI, so it mirrors the id space rather than including the
// sim's internal header.
const char* behavior_name(int32_t behavior) {
  switch (behavior) {
    case 0: return "Idle";
    case 1: return "Roam";
    case 2: return "Buy";
    case 3: return "GoHome";
    case 4: return "VisitTavern";
    case 5: return "Combat";
    default: return "-";
  }
}

const char* command_name(int32_t kind) {
  switch (kind) {
    case GAME_COMMAND_PLACE_BUILDING: return "PlaceBuilding";
    case GAME_COMMAND_RECRUIT_HERO: return "RecruitHero";
    case GAME_COMMAND_DESTROY_BUILDING: return "DestroyBuilding";
    case GAME_COMMAND_MOVE_TO: return "MoveTo";
    case GAME_COMMAND_ENTER_BUILDING: return "EnterBuilding";
    case GAME_COMMAND_ENTER_HOME: return "EnterHome";
    case GAME_COMMAND_BUY: return "Buy";
    case GAME_COMMAND_ATTACK: return "Attack";
    case GAME_COMMAND_SET_BEHAVIOR: return "SetBehavior";
    default: return "?";
  }
}

// The noiser hero brain, if we can read it. Empty means "run the C++ town
// brain" -- the parallel-brains model: a missing or broken script is not fatal,
// the sim just falls back (and reports noiser_bugs in the panel).
std::string load_brain_script() {
  const char* env = std::getenv("BADLANDS_BRAIN_SCRIPT");
  const std::string path = env ? env : "scripts/brains/hero.noiser";
  std::ifstream file(path);
  if (!file.good()) {
    spdlog::warn("AiSandboxView: no brain script at '{}' -- using the C++ town brain",
                 path);
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

AiSandboxView::~AiSandboxView() {
  if (game_) {
    game_destroy(game_);
    game_ = nullptr;
  }
}

bool AiSandboxView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("AiSandboxView::Initialize: MaterialLibrary init failed");
    return false;
  }

  ApplyEnvironment();

  arena_ = build_arena(kSandboxArena);
  SeedTown();
  BuildScene();

  // Frame the camera once, here (the framing is aspect-independent -- see
  // FrameCamera). OnResize only refreshes camera_.aspect afterwards.
  FrameCamera();

  if (!matlib_.ok()) {
    spdlog::error(
        "AiSandboxView::Initialize: material pack(s) failed to load");
    return false;
  }
  return true;
}

void AiSandboxView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void AiSandboxView::SeedTown() {
  const std::string brain = load_brain_script();
  game_ = game_create(brain.empty() ? nullptr : brain.c_str());

  // Everything goes through game_dispatch, so the seed is itself a logged
  // command sequence -- (initial config, seed, command log) still reproduces
  // this world.
  for (const TownBuilding& b : kTown) {
    GameAction place{GAME_ACTION_PLACE_BUILDING, 0, b.x, b.z,
                     static_cast<int32_t>(b.kind), b.rotation_index};
    const int64_t id = game_dispatch(game_, &place);
    if (id < 0) {
      spdlog::warn("AiSandboxView::SeedTown: placing kind {} at ({}, {}) failed",
                   static_cast<int32_t>(b.kind), b.x, b.z);
      continue;
    }
    if (b.kind != GAME_BUILDING_FREE_COMPANY_QUARTERS) continue;
    for (int i = 0; i < kSeedHeroesPerGuild; ++i) {
      GameAction recruit{GAME_ACTION_RECRUIT_HERO, static_cast<uint32_t>(id),
                         0.0f, 0.0f, 0, 0};
      if (game_dispatch(game_, &recruit) < 0) {
        spdlog::warn("AiSandboxView::SeedTown: recruit {} from guild {} failed", i,
                     id);
      }
    }
  }

  char_rows_.resize(kMaxCharacterRows);
  building_rows_.resize(kMaxBuildingRows);
  cmd_rows_.resize(kMaxCommandRows);
}

void AiSandboxView::BuildScene() {
  // Fresh graph: re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path (SceneGraph's constructor resets sun/ambient to its own defaults).
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  // Floor covers the whole arena footprint (interior + wall ring) with
  // headroom; scales with the configured arena size instead of a fixed
  // constant.
  const float full_x = static_cast<float>(arena_.accessible.x + 2);
  const float full_z = static_cast<float>(arena_.accessible.y + 2);
  const float floor_size = std::max(full_x, full_z) + 4.0f;
  AddFloor(scene_, floor_size, matlib_.SolidColor(blockout::kArenaGray, blockout::kArenaRoughness),
           floor_size / kFloorUvRepeatSpacing);

  AddWalls();
  AddBuildings();
  CreateUnitCapsules();
}

void AiSandboxView::AddWalls() {
  const DeferredMaterial wall_mat =
      matlib_.SolidColor(blockout::kArenaGray, blockout::kArenaRoughness);

  int index = 0;
  for (const glm::ivec2& tile : arena_.wall_tiles) {
    const glm::vec2 center = arena_tile_center(tile);
    // GenerateCube is centered at the origin -- translate up by half the
    // height so the block's base sits on the y=0 floor.
    const glm::mat4 transform = glm::translate(
        glm::mat4(1.0f), glm::vec3(center.x, kWallHalfHeight, center.y));

    auto cube = GenerateCube(
        glm::vec3(kWallHalfFootprint, kWallHalfHeight, kWallHalfFootprint));

    const std::string name = "wall_" + std::to_string(index++);
    AddMeshEntity(scene_, name.c_str(), std::move(cube), wall_mat, transform);
  }
}

void AiSandboxView::AddBuildings() {
  if (!game_) return;

  // Blockout building parts: the same BuildBuildingParts assembly the detailed
  // path uses (building_scene.cpp), but with the flat debug palette instead of
  // PBR packs -- the sandbox is a greybox view of the sim, not a beauty shot.
  const DeferredMaterial wall_mat =
      matlib_.SolidColor(blockout::kWall, blockout::kBuildingRoughness);
  const DeferredMaterial roof_mat =
      matlib_.SolidColor(blockout::kRoof, blockout::kBuildingRoughness);

  const uint32_t total =
      game_buildings(game_, building_rows_.data(), kMaxBuildingRows);
  const uint32_t count = std::min(total, kMaxBuildingRows);
  int part_index = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const GameBuildingState& b = building_rows_[i];
    const BuildingVisual bv = building_visual(static_cast<GameBuildingKind>(b.kind));
    const GameRenderBox box = game_render_box(b.kind, /*rotation_index=*/0);
    const GameRenderBox placed = game_render_box(b.kind, b.rotation_index);

    const glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(b.center_x, 0.0f, b.center_z)) *
        glm::rotate(glm::mat4(1.0f), placed.yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));

    for (BuildingPart& part :
         BuildBuildingParts(box.size_x, box.size_z, bv.height, bv.roof)) {
      const std::string name = "building_part_" + std::to_string(part_index++);
      AddMeshEntity(scene_, name.c_str(), std::move(part.mesh),
                    part.kind == BuildingPartKind::Wall ? wall_mat : roof_mat,
                    transform);
    }
  }
}

void AiSandboxView::CreateUnitCapsules() {
  // A fixed pool: one capsule node per possible snapshot row, parked out of
  // sight (scale 0) until SyncUnits places it. Sized once so a hero spawning or
  // dying is a transform write, never a scene rebuild.
  const DeferredMaterial capsule_mat =
      matlib_.SolidColor(blockout::kCapsuleRed, blockout::kCapsuleRoughness);

  capsule_nodes_.clear();
  capsule_nodes_.reserve(kMaxCharacterRows);
  for (uint32_t i = 0; i < kMaxCharacterRows; ++i) {
    // GenerateCapsule's base is already at y=0 (see primitive_mesh_builders.hpp).
    auto capsule = GenerateCapsule(kCapsuleRadius, kCapsuleCylinderHeight, 16);
    const std::string name = "unit_" + std::to_string(i);
    const NodeHandle node =
        AddMeshEntity(scene_, name.c_str(), std::move(capsule), capsule_mat);
    scene_.SetScale(node, glm::vec3(0.0f));
    capsule_nodes_.push_back(node);
  }
}

void AiSandboxView::SyncUnits() {
  if (!game_) return;

  const uint32_t total = game_state(game_, char_rows_.data(), kMaxCharacterRows);
  const uint32_t count = std::min(total, kMaxCharacterRows);
  for (uint32_t i = 0; i < capsule_nodes_.size(); ++i) {
    const bool live = i < count && char_rows_[i].inside_building_id < 0;
    if (!live) {
      scene_.SetScale(capsule_nodes_[i], glm::vec3(0.0f));
      continue;
    }
    const GameCharacterState& c = char_rows_[i];
    scene_.SetScale(capsule_nodes_[i], glm::vec3(1.0f));
    scene_.SetPosition(capsule_nodes_[i], glm::vec3(c.pos_x, 0.0f, c.pos_z));
  }

  command_log_total_ = game_command_log(game_, cmd_rows_.data(), kMaxCommandRows);
}

void AiSandboxView::FrameCamera() {
  gamecam_.focus = glm::vec3(0.0f);
  gamecam_.pitch_deg = 55.0f;

  // Full arena footprint including the 1-tile wall ring on every side.
  const float half_x = 0.5f * static_cast<float>(arena_.accessible.x + 2);
  const float half_z = 0.5f * static_cast<float>(arena_.accessible.y + 2);

  // Empirically-derived coefficients (world units of visible ground extent
  // per world unit of camera height) for GameCameraController's fixed
  // pitch_deg=55 frustum at a 16:9-ish aspect: at height=15 the visible
  // ground spans x in [-19.0, 19.0] and z in [-13.0, +7.2] (the near/south
  // edge at +7.2 is the tighter constraint -- the tilted-down view
  // foreshortens it more than the far/north edge). Visible extent scales
  // linearly with height (same eye-ray angles), so height = extent /
  // coefficient inverts them; +25% margin covers narrower aspect ratios and
  // interactive window resizing.
  constexpr float kXCoeff = 19.0f / 15.0f;
  constexpr float kZNearCoeff = 7.2f / 15.0f;  // the tighter (south) edge
  const float height_for_x = half_x / kXCoeff;
  const float height_for_z = half_z / kZNearCoeff;
  gamecam_.height = 1.25f * std::max(height_for_x, height_for_z);

  gamecam_.UpdateCamera(camera_);
}

void AiSandboxView::HandleEvent(const SDL_Event& event, int /*width*/,
                                int /*height*/) {
  // Fixed-angle camera: only zoom is mouse-driven (wheel + trackpad, which SDL
  // reports as the same event with fractional deltas). Key panning is read
  // directly from Update()'s keyboard_state snapshot instead of per-event.
  if (event.type != SDL_EVENT_MOUSE_WHEEL) return;
  if (ImGui::GetIO().WantCaptureMouse) return;
  glm::vec2 screen;
  if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
  ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel),
               glm::vec2(event.wheel.mouse_x, event.wheel.mouse_y), screen);
}

void AiSandboxView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Fixed-interval sim: advance the clock from real dt * speed, then run
  // game_tick(kTickDt) until we catch up to the tick target. The speed control
  // therefore accelerates the day/night loop WITHOUT changing the fixed rate
  // the sim itself sees -- determinism is preserved (the sim never observes a
  // variable dt), only how fast we feed it changes.
  sim_clock_.Advance(static_cast<double>(dt));
  const unsigned long long tick_target = sim_clock_.TickTarget();
  int budget = kMaxSimTicksPerFrame;
  while (sim_ticks_done_ < tick_target && budget-- > 0) {
    if (game_) game_tick(game_, static_cast<float>(kTickDt));
    ++sim_ticks_done_;
  }

  SyncUnits();

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
}

void AiSandboxView::DrawUI() {
  if (!scene_renderer_) return;

  // NOTE(lighting): on any frame the editor changes env_, ApplyEnvironment
  // re-derives the full sky (6 faces x face x face radiance), a 2048-sample SH
  // projection, and a GPU cube rebuild + IBL re-prefilter next frame -- to be
  // debounced / made incremental in the future lighting commit.
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }

  DrawInspector();
}

void AiSandboxView::DrawInspector() {
  ImGui::Begin("Sim");
  if (!game_) {
    ImGui::TextUnformatted("no sim");
    ImGui::End();
    return;
  }

  GameWorldState world{};
  game_world(game_, &world);
  GameStats stats{};
  game_stats(game_, &stats);

  // --- clock -----------------------------------------------------------
  ImGui::Text("Day %u  %02d:%02d  %s", world.day,
              static_cast<int>(world.time_of_day * 24.0f),
              static_cast<int>(world.time_of_day * 24.0f * 60.0f) % 60,
              world.is_night ? "(night)" : "(day)");
  ImGui::Text("t = %lld ms   tick %llu", static_cast<long long>(world.world_millis),
              static_cast<unsigned long long>(stats.ticks));
  ImGui::SliderFloat("speed", &sim_clock_.speed, 0.0f, 60.0f, "%.0fx");
  ImGui::SameLine();
  if (ImGui::SmallButton("1x")) sim_clock_.speed = 1.0f;
  ImGui::Text("noiser bugs: %u   script intents: %llu", stats.noiser_bugs,
              static_cast<unsigned long long>(stats.script_intents));

  // --- heroes ----------------------------------------------------------
  ImGui::SeparatorText("Heroes");
  const uint32_t total = game_state(game_, char_rows_.data(), kMaxCharacterRows);
  const uint32_t count = std::min(total, kMaxCharacterRows);
  if (ImGui::BeginTable("heroes", 7, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("name");
    ImGui::TableSetupColumn("behavior");
    ImGui::TableSetupColumn("goal");
    ImGui::TableSetupColumn("fatigue");
    ImGui::TableSetupColumn("boredom");
    ImGui::TableSetupColumn("home");
    ImGui::TableSetupColumn("inside");
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < count; ++i) {
      const GameCharacterState& c = char_rows_[i];
      if (c.name[0] == '\0') continue;  // not a hero
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(c.name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(behavior_name(c.behavior));
      ImGui::TableNextColumn();
      // What it is walking toward right now, and how far along the route.
      if (c.goal_kind == 0) {
        ImGui::TextUnformatted("-");
      } else {
        ImGui::Text("(%.1f, %.1f) +%d", c.goal_x, c.goal_z, c.path_waypoints);
      }
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", c.fatigue);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", c.boredom);
      ImGui::TableNextColumn();
      ImGui::Text("%d", c.home_building_id);
      ImGui::TableNextColumn();
      ImGui::Text("%d", c.inside_building_id);
    }
    ImGui::EndTable();
  }

  // --- the trace of record ---------------------------------------------
  ImGui::SeparatorText("Command log");
  ImGui::Text("%u applied (showing last %u)", command_log_total_,
              std::min(command_log_total_, kMaxCommandRows));
  const uint32_t shown = std::min(command_log_total_, kMaxCommandRows);
  if (ImGui::BeginChild("cmdlog", ImVec2(0.0f, 160.0f))) {
    for (uint32_t i = 0; i < shown; ++i) {
      const GameCommandRecord& r = cmd_rows_[i];
      ImGui::Text("%-14s actor=%s (%.1f, %.1f) a=%d", command_name(r.kind),
                  r.actor == UINT32_MAX ? "player" : std::to_string(r.actor).c_str(),
                  r.point_x, r.point_z, r.param_a);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

void AiSandboxView::OnResize(int width, int height) {
  // Only refresh the aspect. FrameCamera() (run once in Initialize) must NOT
  // be called here: it resets gamecam_.focus to the origin, which would
  // discard any WASD pan on every window resize. The framing is
  // aspect-independent (see FrameCamera's coefficient comment), so nothing
  // needs re-framing on resize.
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
