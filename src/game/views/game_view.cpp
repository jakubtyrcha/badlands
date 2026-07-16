#include "game/views/game_view.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/fixed_timestep.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/building_catalog.h"
#include "game/scene/building_scene.h"

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

// Day/night: the sky cube + SH + IBL prefilter are re-baked at most once per
// this much REAL time (the directional light + shadows still move every frame).
// A real-time throttle bounds the ~per-frame HW cube bake cost independent of
// framerate and sim speed; an Update(0) never re-bakes. Seeks/config edits
// force an immediate re-bake.
constexpr double kRebakeIntervalSeconds = 1.0 / 20.0;  // ~20 Hz sky/IBL refresh

// Real seconds for one in-game day (overrides SimClock's 5-minute default).
// Short for now so the whole cycle is quick to see.
constexpr float kRealSecondsPerDay = 16.0f;

// Where the live day/night clock starts (t01: 0.30 = mid-morning).
constexpr float kInitialTimeOfDay = 0.30f;

constexpr const char* kFloorPackDir =
    "assets/materials/monastery_stone_floor_1k";
// Repeat the floor pack roughly once per 2 world units instead of stretching
// one copy across the whole floor.
constexpr float kFloorUvRepeatSpacing = 2.0f;

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

  // Fixed-angle game camera framing the demo town: the Castle sits at the
  // origin and the demo buildings spread to +-12 in X / up to +10 in Z (see
  // PlaceDemoBuildings), so pull back further than GameCameraController's
  // defaults (pitch 55deg/height 30) to keep the whole spread inside the
  // 45deg-FOV frustum.
  gamecam_.focus = glm::vec3(0.0f, 0.0f, 4.0f);
  gamecam_.pitch_deg = 50.0f;
  gamecam_.height = 42.0f;
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
                           scene_context_);
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

  constexpr float kFloorSize = 80.0f;
  AddFloor(scene_, matlib_, kFloorSize, kFloorPackDir,
           kFloorSize / kFloorUvRepeatSpacing);

  if (building_rows_.size() < kMaxBuildingRows) {
    building_rows_.resize(kMaxBuildingRows);
  }
  const uint32_t total =
      game_buildings(game_, building_rows_.data(), kMaxBuildingRows);
  if (total > kMaxBuildingRows) {
    spdlog::warn("GameView::BuildScene: {} buildings truncated to {}", total,
                kMaxBuildingRows);
  }
  const uint32_t count = std::min(total, kMaxBuildingRows);

  for (uint32_t i = 0; i < count; ++i) {
    const GameBuildingState& b = building_rows_[i];
    AddBuildingToScene(scene_, matlib_, static_cast<GameBuildingKind>(b.kind),
                       glm::vec2(b.center_x, b.center_z),
                       yaw_from_rotation_index(b.rotation_index));
  }
}

void GameView::HandleEvent(const SDL_Event& /*event*/, int /*width*/,
                           int /*height*/) {
  // Fixed-angle camera: no mouse orbit/zoom to wire up. Key panning is
  // read directly from Update()'s keyboard_state snapshot instead of
  // per-event, so there is nothing for this view to do here.
}

void GameView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Advance the single time source: real dt -> * sim speed -> sim time. Both
  // the day/night cycle and the fixed-rate game logic derive from this clock,
  // so they run together at the current speed, independent of framerate.
  const double real_dt = static_cast<double>(dt);
  sim_clock_.Advance(real_dt);

  // Fixed-interval game logic: run game_tick(kTickDt) until we've caught up to
  // the clock's tick target. Bounded (real dt is clamped in Advance); the
  // budget is pure spiral-of-death safety. Ticks scale with sim speed because
  // the target is derived from sim time.
  const unsigned long long tick_target = sim_clock_.TickTarget();
  int budget = kMaxSimTicksPerFrame;
  while (sim_ticks_done_ < tick_target && budget-- > 0) {
    if (game_) game_tick(game_, static_cast<float>(kTickDt));
    ++sim_ticks_done_;
  }

  // Day/night rendering, driven by the clock's time-of-day. The sky re-bake is
  // throttled by real time (accumulated here); the directional light moves
  // every frame inside UpdateDaylight.
  rebake_accum_ += real_dt;
  UpdateDaylight();

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
    if (dir.x != 0.0f || dir.y != 0.0f) {
      gamecam_.Pan(glm::normalize(dir) * gamecam_.pan_speed * dt);
    }
  }

  gamecam_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
}

void GameView::SeekToTimeOfDay(float t01) {
  // Discontinuous jump (headless capture / editor scrub): move the clock and
  // sync the game-tick counter so we do NOT simulate the skipped time, then
  // re-bake immediately so a single frame is correct.
  sim_clock_.SeekTimeOfDay(t01);
  sim_ticks_done_ = sim_clock_.TickTarget();
  ApplyDaylightNow();
}

void GameView::DrawUI() {
  if (!scene_renderer_) return;

  // Daylight + sim-time controls + the shared renderer-debug selectors + FPS.
  // Editing a DaylightConfig value forces an immediate sky re-bake so the
  // change is visible without waiting for the throttle.
  ImGui::Begin("Daylight / Sim");

  // Sim speed (0 = paused, 1/2/4x). Drives both the day/night cycle and the
  // game logic via the shared SimClock.
  ImGui::Text("Speed:");
  const float kSpeeds[] = {0.0f, 1.0f, 2.0f, 4.0f};
  const char* kSpeedLabels[] = {"Pause", "1x", "2x", "4x"};
  for (int i = 0; i < 4; ++i) {
    ImGui::SameLine();
    if (ImGui::RadioButton(kSpeedLabels[i], sim_clock_.speed == kSpeeds[i])) {
      sim_clock_.speed = kSpeeds[i];
    }
  }

  // Time-of-day scrubber + day counter.
  float t01 = sim_clock_.TimeOfDay();
  if (ImGui::SliderFloat("Time of day", &t01, 0.0f, 0.9999f)) {
    SeekToTimeOfDay(t01);
  }
  ImGui::Text("Day %d  |  %05.2f h", sim_clock_.DayCounter(), t01 * 24.0f);
  ImGui::SliderFloat("Real sec / day", &sim_clock_.real_seconds_per_day, 1.0f, 600.0f);

  bool cfg_changed = false;
  cfg_changed |= ImGui::SliderFloat("Turbidity", &daylight_cfg_.turbidity, 1.0f, 10.0f);
  cfg_changed |= ImGui::SliderFloat("Ground albedo", &daylight_cfg_.ground_albedo, 0.0f, 1.0f);
  cfg_changed |= ImGui::SliderFloat("Sky exposure", &daylight_cfg_.sky_exposure, 0.001f, 0.3f, "%.3f");
  cfg_changed |= ImGui::SliderFloat("Sun intensity", &daylight_cfg_.sun_intensity_max, 0.0f, 10.0f);
  cfg_changed |= ImGui::ColorEdit3("Moon color", &daylight_cfg_.moon_color.x);
  cfg_changed |= ImGui::SliderFloat("Moon intensity", &daylight_cfg_.moon_intensity, 0.0f, 0.2f, "%.3f");
  cfg_changed |= ImGui::SliderFloat("Ease minutes", &daylight_cfg_.ease_ingame_minutes, 1.0f, 120.0f);
  cfg_changed |= ImGui::Checkbox("Moon disc", &daylight_cfg_.moon_disc);
  if (cfg_changed) force_rebake_ = true;
  EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
  EditorUI::DrawShadowDebugSelector(*scene_renderer_);
  EditorUI::DrawStats(dt_);
  ImGui::End();

  if (!game_) return;

  GameWorldState world{};
  game_world(game_, &world);

  // Reused member buffer (kMaxBuildingRows capacity) -- avoids a per-frame
  // heap allocation for the read-back building rows.
  if (building_rows_.size() < kMaxBuildingRows) {
    building_rows_.resize(kMaxBuildingRows);
  }
  const uint32_t total =
      game_buildings(game_, building_rows_.data(), kMaxBuildingRows);
  const uint32_t count = std::min(total, kMaxBuildingRows);

  ImGui::Begin("World");
  ImGui::Text("Gold: %u", world.gold);
  ImGui::Text("Buildings: %u", total);
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
}

}  // namespace badlands
