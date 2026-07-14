#include "game/views/game_view.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/scene/building_scene.h"

namespace badlands {

namespace {

// Creates a 1x1 solid-color RGBA8Unorm texture view (procedural floor
// albedo/roughness). Same pattern as ModelViewerView's CreateSolid1x1
// (src/viewer/model_viewer_view.cpp) -- duplicated per that file's comment:
// a small file-local utility, not a shared deliverable.
wgpu::TextureView CreateSolid1x1(wgpu::Device device, wgpu::Queue queue, uint8_t r,
                                 uint8_t g, uint8_t b, uint8_t a) {
  wgpu::TextureDescriptor desc;
  desc.size = {1, 1, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  wgpu::Texture tex = device.CreateTexture(&desc);

  const uint8_t data[4] = {r, g, b, a};
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = tex;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = 4;
  layout.rowsPerImage = 1;
  wgpu::Extent3D extent = {1, 1, 1};
  queue.WriteTexture(&dst, data, sizeof(data), &layout, &extent);
  return tex.CreateView();
}

// GameBuildingKind -> display label for the World panel's building list, in
// enum declaration order (badlands_game.h). Mirrors ModelViewerView's
// kBuildingLabels (src/viewer/model_viewer_view.cpp) -- file-local, not
// factored into a shared header since only these two views need it so far.
struct BuildingLabel {
  GameBuildingKind kind;
  const char* label;
};
constexpr BuildingLabel kBuildingLabels[] = {
    {GAME_BUILDING_CASTLE, "Castle"},
    {GAME_BUILDING_FREE_COMPANY_QUARTERS, "Free Company Quarters"},
    {GAME_BUILDING_HUNTERS_CAMP, "Hunters Camp"},
    {GAME_BUILDING_THIEVES_DEN, "Thieves Den"},
    {GAME_BUILDING_SCRIPTORIUM, "Scriptorium"},
    {GAME_BUILDING_TAVERN, "Tavern"},
    {GAME_BUILDING_APOTHECARY, "Apothecary"},
    {GAME_BUILDING_WATCHTOWER, "Watchtower"},
    {GAME_BUILDING_HOUSE, "House"},
    {GAME_BUILDING_SEWER, "Sewer"},
};
static_assert(sizeof(kBuildingLabels) / sizeof(kBuildingLabels[0]) ==
                 GAME_BUILDING_KIND_COUNT,
             "kBuildingLabels must cover every GameBuildingKind");

const char* LabelFor(int32_t kind) {
  for (const BuildingLabel& b : kBuildingLabels) {
    if (static_cast<int32_t>(b.kind) == kind) return b.label;
  }
  return "?";
}

// rotation_index 0..3 -> 0/45/90/135deg world yaw about Y (badlands_game.h's
// GamePlacementDesc convention) -- see building_scene.h's AddBuildingToScene
// comment for why this is exact for 0/2 and an approximation for 1/3.
float yaw_from_rotation_index(int32_t rotation_index) {
  return glm::radians(static_cast<float>(rotation_index) * 45.0f);
}

// Up to this many rows are read from game_buildings/game_state per call --
// comfortably above this stage's Castle + 4 demo buildings.
constexpr uint32_t kMaxBuildingRows = 64;

}  // namespace

GameView::~GameView() {
  if (game_) {
    game_destroy(game_);
    game_ = nullptr;
  }
}

void GameView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen);

  // Neutral gray floor (same albedo/roughness values + rationale as
  // ModelViewerView's AddFloor -- src/viewer/model_viewer_view.cpp).
  floor_albedo_view_ = CreateSolid1x1(ctx.device, ctx.queue, 110, 110, 110, 255);
  floor_roughness_view_ = CreateSolid1x1(ctx.device, ctx.queue, 229, 229, 229, 255);
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  floor_sampler_ = ctx.device.CreateSampler(&samp_desc);

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
  // Fresh graph: re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path (SceneGraph's constructor resets sun/ambient to its own defaults).
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor();

  std::vector<GameBuildingState> rows(kMaxBuildingRows);
  const uint32_t total = game_buildings(game_, rows.data(), kMaxBuildingRows);
  if (total > kMaxBuildingRows) {
    spdlog::warn("GameView::BuildScene: {} buildings truncated to {}", total,
                kMaxBuildingRows);
  }
  rows.resize(std::min(total, kMaxBuildingRows));

  for (const GameBuildingState& b : rows) {
    AddBuildingToScene(scene_, matlib_, static_cast<GameBuildingKind>(b.kind),
                       glm::vec2(b.center_x, b.center_z),
                       yaw_from_rotation_index(b.rotation_index));
  }
}

void GameView::AddFloor() {
  auto quad = GenerateQuadTexturedMesh(80.0f);

  // GenerateQuadTexturedMesh spans X/Y at Z=0 with normal +Z; rotate -90deg
  // about X so the normal becomes +Y (up) and the quad spans X/Z at Y=0.
  const glm::mat4 transform =
      glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

  InstanceParams floor_params;
  floor_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = floor_albedo_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });
  floor_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "roughness",
      .view = floor_roughness_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });

  DeferredMaterial floor_mat{.factory = matlib_.factory(), .params = floor_params};
  AddMeshEntity(scene_, "floor", std::move(quad), floor_mat, transform);
}

void GameView::HandleEvent(const SDL_Event& /*event*/, int /*width*/,
                           int /*height*/) {
  // Fixed-angle camera: no mouse orbit/zoom to wire up. Key panning is
  // read directly from Update()'s keyboard_state snapshot instead of
  // per-event, so there is nothing for this view to do here.
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

  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }

  if (!game_) return;

  GameWorldState world{};
  game_world(game_, &world);

  std::vector<GameBuildingState> rows(kMaxBuildingRows);
  const uint32_t total = game_buildings(game_, rows.data(), kMaxBuildingRows);
  rows.resize(std::min(total, kMaxBuildingRows));

  ImGui::Begin("World");
  ImGui::Text("Gold: %u", world.gold);
  ImGui::Text("Buildings: %u", total);
  ImGui::Separator();
  for (const GameBuildingState& b : rows) {
    ImGui::Text("#%u %-24s (%.1f, %.1f)", b.id, LabelFor(b.kind), b.center_x,
               b.center_z);
  }
  ImGui::End();
}

void GameView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
