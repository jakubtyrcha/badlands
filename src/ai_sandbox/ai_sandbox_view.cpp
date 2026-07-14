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
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/material_pack.h"

namespace badlands {

namespace {

// Creates a 1x1 solid-color RGBA8Unorm texture view (procedural floor/
// capsule albedo/roughness). Same pattern as GameView/ModelViewerView's
// CreateSolid1x1 -- a small file-local utility, not a shared deliverable.
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

// Wall block footprint/height (world units; tile = 1.0 world unit).
constexpr float kWallHalfFootprint = 0.5f;
constexpr float kWallHalfHeight = 0.6f;  // 1.2 tall

// Capsule dimensions (world units).
constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleCylinderHeight = 0.6f;

}  // namespace

void AiSandboxView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen);

  // Neutral gray floor (same albedo/roughness values + rationale as
  // GameView/ModelViewerView's AddFloor).
  floor_albedo_view_ = CreateSolid1x1(ctx.device, ctx.queue, 110, 110, 110, 255);
  floor_roughness_view_ = CreateSolid1x1(ctx.device, ctx.queue, 229, 229, 229, 255);
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  floor_sampler_ = ctx.device.CreateSampler(&samp_desc);

  // Capsule solid colors: red + blue, mid roughness (~0.55) shared by both.
  // Normal falls back to the normalmapped factory's flat-normal default.
  capsule_red_albedo_view_ = CreateSolid1x1(ctx.device, ctx.queue, 200, 30, 30, 255);
  capsule_blue_albedo_view_ = CreateSolid1x1(ctx.device, ctx.queue, 30, 60, 200, 255);
  capsule_roughness_view_ = CreateSolid1x1(ctx.device, ctx.queue, 140, 140, 140, 255);

  ApplyEnvironment();

  arena_ = build_arena();
  BuildScene();

  FrameCamera();
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
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor();
  AddWalls();
  AddCapsules();
}

void AiSandboxView::AddFloor() {
  // Covers the whole arena footprint (interior + wall ring) with headroom;
  // scales with the configured arena size instead of a fixed constant.
  const float full_x = static_cast<float>(arena_.accessible.x + 2);
  const float full_z = static_cast<float>(arena_.accessible.y + 2);
  const float size = std::max(full_x, full_z) + 4.0f;

  auto quad = GenerateQuadTexturedMesh(size);

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

void AiSandboxView::AddWalls() {
  const MaterialPack pack = material_pack(MaterialId::RockWall);
  const DeferredMaterial wall_mat = matlib_.Get(pack.dir, pack.base);

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

  InstanceParams red_params;
  red_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = capsule_red_albedo_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });
  red_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "roughness",
      .view = capsule_roughness_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });
  const DeferredMaterial red_mat{.factory = matlib_.factory(), .params = red_params};

  InstanceParams blue_params;
  blue_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = capsule_blue_albedo_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });
  blue_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "roughness",
      .view = capsule_roughness_view_,
      .sampler = floor_sampler_,
      .type = TextureType::k2D,
  });
  const DeferredMaterial blue_mat{.factory = matlib_.factory(), .params = blue_params};

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
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  FrameCamera();
}

}  // namespace badlands
