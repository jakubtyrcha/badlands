#include "viewer/model_viewer_view.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "engine/rendering/geometry/extrusion_mesh_builder.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/building_catalog.h"
#include "game/material_pack.h"

namespace badlands {

namespace {

// Creates a 1x1 solid-color RGBA8Unorm texture view (procedural floor
// albedo/roughness -- no JPEG to load). Same pattern as PlaceholderView's
// CreateSolid1x1 (src/engine/app/placeholder_view.cpp), duplicated here since
// it's a small, file-local utility, not part of the S2.E deliverables (the
// shared helper that task adds is scene_build's AddMeshEntity).
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

// GameBuildingKind -> display label, in enum declaration order (badlands_game.h).
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

}  // namespace

void ModelViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen);

  // Neutral gray floor: light-gray albedo, default flat normal (no override
  // -- the factory's flat_normal default), high roughness (~0.9, matte).
  // Albedo 110/255 (not a brighter/whiter gray): the floor's normal points
  // straight up, near-parallel to the default LightEnvironment's high sun
  // (src/engine/rendering/light_environment.hpp's default sun_direction is
  // ~26deg off zenith) -- an upward face gets close to the highest NdotL of
  // any surface in the scene, so a brighter albedo here clips to white after
  // tonemapping even though the same value reads as a normal mid-gray on a
  // vertical wall (see the S2.E task report for the empirical sweep).
  floor_albedo_view_ = CreateSolid1x1(ctx.device, ctx.queue, 110, 110, 110, 255);
  floor_roughness_view_ = CreateSolid1x1(ctx.device, ctx.queue, 229, 229, 229, 255);
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  floor_sampler_ = ctx.device.CreateSampler(&samp_desc);

  BuildCatalog();
  if (catalog_.empty()) {
    spdlog::error("ModelViewerView::Initialize: empty prefab catalog");
    return;
  }
  prefab_index_ =
      std::clamp(prefab_index_, 0, static_cast<int>(catalog_.size()) - 1);

  ApplyEnvironment();
  RebuildScene();
}

void ModelViewerView::BuildCatalog() {
  catalog_.clear();
  catalog_.push_back({.label = "Rock A",
                      .category = PrefabCategory::Rock,
                      .rock_kind = GamePloppableKind::RockA});
  catalog_.push_back({.label = "Rock B",
                      .category = PrefabCategory::Rock,
                      .rock_kind = GamePloppableKind::RockB});
  catalog_.push_back({.label = "Rock C",
                      .category = PrefabCategory::Rock,
                      .rock_kind = GamePloppableKind::RockC});
  for (const BuildingLabel& b : kBuildingLabels) {
    catalog_.push_back({.label = b.label,
                        .category = PrefabCategory::Building,
                        .building_kind = b.kind});
  }
}

void ModelViewerView::ApplyEnvironment() {
  // Rebuilds the sky cube + SH ambient + sun from env_ into scene_context_
  // (bumps skybox_generation so SceneRenderer re-prefilters the IBL cube next
  // frame), then re-mirrors the derived lighting into scene_ so its
  // per-frame SyncToRegistry (Update) rewrites the SAME sun/ambient values
  // into scene_context_ instead of clobbering them with SceneGraph defaults
  // (same pattern as PlaceholderView -- see its Initialize/DrawUI comments).
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void ModelViewerView::RebuildScene() {
  // Fresh graph: SceneGraph has no "clear all nodes" call, and a moved-from
  // default-constructed graph is the cheapest way to drop every prior
  // prefab's entities. Its constructor resets sun/ambient to SceneGraph's own
  // defaults, so re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path.
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor();

  const Aabb bounds = AddPrefab(catalog_[prefab_index_]);
  const glm::vec3 center = bounds.Center();
  const float radius = glm::length(bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
}

void ModelViewerView::AddFloor() {
  auto quad = GenerateQuadTexturedMesh(40.0f);

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

Aabb ModelViewerView::AddPrefab(const PrefabEntry& entry) {
  if (entry.category == PrefabCategory::Rock) {
    std::vector<glm::vec2> ring = ploppable_local_ring(entry.rock_kind, 0);
    TexturedMeshResult mesh = BuildExtrusionMesh(ring, /*base_y=*/0.0f,
                                                 /*delta_y=*/0.8f, /*shrink=*/0.3f);
    const Aabb bounds = mesh.local_bounds;

    const MaterialPack pack = material_pack(MaterialId::RockWall);
    const DeferredMaterial mat = matlib_.Get(pack.dir, pack.base);
    AddMeshEntity(scene_, "rock", std::move(mesh), mat);
    return bounds;
  }

  const BuildingVisual bv = building_visual(entry.building_kind);
  const GameRenderBox box =
      game_render_box(static_cast<int32_t>(entry.building_kind), /*rotation_index=*/0);
  std::vector<BuildingPart> parts =
      BuildBuildingParts(box.size_x, box.size_z, bv.height, bv.roof);

  Aabb bounds = Aabb::Empty();
  int part_index = 0;
  for (BuildingPart& part : parts) {
    const MaterialId mat_id =
        (part.kind == BuildingPartKind::Wall) ? bv.wall_material : bv.roof_material;
    const MaterialPack pack = material_pack(mat_id);
    const DeferredMaterial mat = matlib_.Get(pack.dir, pack.base);

    bounds = bounds.Union(part.mesh.local_bounds);
    const std::string name = "building_part_" + std::to_string(part_index++);
    AddMeshEntity(scene_, name.c_str(), std::move(part.mesh), mat);
  }
  return bounds;
}

void ModelViewerView::HandleEvent(const SDL_Event& event, int /*width*/,
                                  int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = true;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (left_mouse_down_) {
        orbit_.HandleMouseDrag(event.motion.xrel, event.motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      orbit_.HandleMouseWheel(event.wheel.y);
      break;
    default:
      break;
  }
}

void ModelViewerView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
}

void ModelViewerView::DrawUI() {
  if (!scene_renderer_ || catalog_.empty()) return;

  int selected = prefab_index_;
  ImGui::Begin("Prefab");
  if (ImGui::BeginCombo("Prefab", catalog_[prefab_index_].label.c_str())) {
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
      const bool is_selected = (i == prefab_index_);
      if (ImGui::Selectable(catalog_[i].label.c_str(), is_selected)) {
        selected = i;
      }
      if (is_selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::End();

  if (selected != prefab_index_) {
    prefab_index_ = selected;
    RebuildScene();
  }

  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }
}

void ModelViewerView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
