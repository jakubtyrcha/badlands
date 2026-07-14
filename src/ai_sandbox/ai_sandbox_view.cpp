#include "ai_sandbox/ai_sandbox_view.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/material_pack.h"

namespace badlands {

namespace {

// Capsule solid colors (linear 0..1 RGB) + shared mid roughness, resolved to
// cached deferred materials via MaterialLibrary::SolidColor. Normal falls
// back to the normalmapped factory's flat-normal default.
constexpr glm::vec3 kCapsuleRedRgb{200.0f / 255.0f, 30.0f / 255.0f,
                                   30.0f / 255.0f};
constexpr glm::vec3 kCapsuleBlueRgb{30.0f / 255.0f, 60.0f / 255.0f,
                                    200.0f / 255.0f};
constexpr float kCapsuleRoughness = 140.0f / 255.0f;

// Wall block footprint/height (world units; tile = 1.0 world unit).
constexpr float kWallHalfFootprint = 0.5f;
constexpr float kWallHalfHeight = 0.6f;  // 1.2 tall

constexpr const char* kFloorPackDir =
    "assets/materials/monastery_stone_floor_1k";
// Repeat the floor pack roughly once per 2 world units instead of stretching
// one copy across the whole floor.
constexpr float kFloorUvRepeatSpacing = 2.0f;

// Capsule dimensions (world units).
constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleCylinderHeight = 0.6f;

}  // namespace

bool AiSandboxView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("AiSandboxView::Initialize: MaterialLibrary init failed");
    return false;
  }

  ApplyEnvironment();

  arena_ = build_arena();
  BuildScene();

  // Frame the camera once, here (the framing is aspect-independent -- see
  // FrameCamera). OnResize only refreshes camera_.aspect afterwards.
  FrameCamera();
  return true;
}

void AiSandboxView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void AiSandboxView::BuildScene() {
  // Fresh graph: re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path (SceneGraph's constructor resets sun/ambient to its own defaults).
  // (See GameView::BuildScene's NOTE(lighting) on centralizing this mirror.)
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
  AddFloor(scene_, matlib_, floor_size, kFloorPackDir,
           floor_size / kFloorUvRepeatSpacing);

  AddWalls();
  AddCapsules();
}

void AiSandboxView::AddWalls() {
  const MaterialPack pack = material_pack(MaterialId::RockWall);
  const DeferredMaterial wall_mat = matlib_.Get(pack.dir);

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

void AiSandboxView::AddCapsules() {
  if (arena_.floor_tiles.empty()) {
    spdlog::error(
        "AiSandboxView::AddCapsules: empty arena (build_arena failed its "
        "grid-bounds check) -- skipping capsules");
    return;
  }

  // Pick two interior tiles on opposite sides along X, one margin tile in
  // from the wall ring on either side -- derived from arena_.floor_tiles
  // (not re-deriving build_arena's centering formula) so this stays correct
  // for any configured arena size.
  glm::ivec2 min_tile = arena_.floor_tiles.front();
  glm::ivec2 max_tile = arena_.floor_tiles.front();
  for (const glm::ivec2& t : arena_.floor_tiles) {
    min_tile = glm::min(min_tile, t);
    max_tile = glm::max(max_tile, t);
  }
  const glm::ivec2 tile_a(min_tile.x + 1, 0);
  const glm::ivec2 tile_b(max_tile.x - 1, 0);

  capsule_a_pos_ = arena_tile_center(tile_a);
  capsule_b_pos_ = arena_tile_center(tile_b);

  const DeferredMaterial red_mat =
      matlib_.SolidColor(kCapsuleRedRgb, kCapsuleRoughness);
  const DeferredMaterial blue_mat =
      matlib_.SolidColor(kCapsuleBlueRgb, kCapsuleRoughness);

  // GenerateCapsule's base is already at y=0 (see primitive_mesh_builders.hpp).
  auto capsule_a = GenerateCapsule(kCapsuleRadius, kCapsuleCylinderHeight, 16);
  const glm::mat4 transform_a = glm::translate(
      glm::mat4(1.0f), glm::vec3(capsule_a_pos_.x, 0.0f, capsule_a_pos_.y));
  AddMeshEntity(scene_, "capsule_red", std::move(capsule_a), red_mat, transform_a);

  auto capsule_b = GenerateCapsule(kCapsuleRadius, kCapsuleCylinderHeight, 16);
  const glm::mat4 transform_b = glm::translate(
      glm::mat4(1.0f), glm::vec3(capsule_b_pos_.x, 0.0f, capsule_b_pos_.y));
  AddMeshEntity(scene_, "capsule_blue", std::move(capsule_b), blue_mat, transform_b);
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

void AiSandboxView::HandleEvent(const SDL_Event& /*event*/, int /*width*/,
                                int /*height*/) {
  // Fixed-angle camera: no mouse orbit/zoom to wire up. Key panning is read
  // directly from Update()'s keyboard_state snapshot instead of per-event,
  // so there is nothing for this view to do here.
}

void AiSandboxView::Update(float dt, const bool* keyboard_state) {
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

void AiSandboxView::DrawUI() {
  if (!scene_renderer_) return;

  // NOTE(lighting): on any frame the editor changes env_, ApplyEnvironment
  // re-derives the full sky (6 faces x face x face radiance), a 2048-sample SH
  // projection, and a GPU cube rebuild + IBL re-prefilter next frame -- to be
  // debounced / made incremental in the future lighting commit. No behavior
  // change here.
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }

  ImGui::Begin("Arena");
  ImGui::Text("Accessible: %d x %d", arena_.accessible.x, arena_.accessible.y);
  ImGui::Text("Floor tiles: %zu", arena_.floor_tiles.size());
  ImGui::Text("Wall tiles: %zu", arena_.wall_tiles.size());
  ImGui::Separator();
  ImGui::Text("Capsule (red):  (%.1f, %.1f)", capsule_a_pos_.x, capsule_a_pos_.y);
  ImGui::Text("Capsule (blue): (%.1f, %.1f)", capsule_b_pos_.x, capsule_b_pos_.y);
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
