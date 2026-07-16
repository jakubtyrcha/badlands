#include "game/views/game_view.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/game_camera_controller.hpp"  // ZoomAtCursor
#include "engine/app/sdl_input_util.hpp"
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

  ApplyEnvironment();

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

void GameView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
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
  // AmbientSH} sequence below mirrors scene_context_'s already-derived-from-
  // env_ lighting into the fresh graph (whose constructor otherwise resets
  // sun/ambient to SceneGraph's own defaults, which the per-frame
  // SyncToRegistry would then write back over scene_context_). This same
  // 4-line mirror is repeated at every rebuild/apply site across the three
  // views; it will be centralized in the future lighting refactor. No
  // behavior change now.
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

void GameView::HandleEvent(const SDL_Event& event, int /*width*/,
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

void GameView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

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

void GameView::DrawUI() {
  if (!scene_renderer_) return;

  // NOTE(lighting): on any frame the light-environment editor changes env_,
  // ApplyEnvironment re-derives the full sky (6 faces x face x face radiance),
  // a 2048-sample SH projection, and a GPU cube rebuild + IBL re-prefilter
  // next frame -- to be debounced / made incremental in the future lighting
  // commit. No behavior change here.
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }

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
