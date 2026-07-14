#include "viewer/model_viewer_view.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/extrusion_mesh_builder.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/building_catalog.h"
#include "game/material_pack.h"
#include "game/scene/building_scene.h"

namespace badlands {

bool ModelViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error(
        "ModelViewerView::Initialize: MaterialLibrary init failed");
    return false;
  }

  BuildCatalog();
  if (catalog_.empty()) {
    spdlog::error("ModelViewerView::Initialize: empty prefab catalog");
    return false;
  }
  prefab_index_ =
      std::clamp(prefab_index_, 0, static_cast<int>(catalog_.size()) - 1);

  ApplyEnvironment();
  RebuildScene();
  return true;
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
  for (int32_t k = 0; k < GAME_BUILDING_KIND_COUNT; ++k) {
    const auto kind = static_cast<GameBuildingKind>(k);
    catalog_.push_back({.label = building_label(kind),
                        .category = PrefabCategory::Building,
                        .building_kind = kind});
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

  AddGrayFloor(scene_, matlib_, 40.0f);

  const Aabb bounds = AddPrefab(catalog_[prefab_index_]);
  const glm::vec3 center = bounds.Center();
  const float radius = glm::length(bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
}

Aabb ModelViewerView::AddPrefab(const PrefabEntry& entry) {
  if (entry.category == PrefabCategory::Rock) {
    std::vector<glm::vec2> ring = ploppable_local_ring(entry.rock_kind, 0);
    TexturedMeshResult mesh = BuildExtrusionMesh(ring, /*base_y=*/0.0f,
                                                 /*delta_y=*/0.8f, /*shrink=*/0.3f);
    const Aabb bounds = mesh.local_bounds;

    const MaterialPack pack = material_pack(MaterialId::RockWall);
    const DeferredMaterial mat = matlib_.Get(pack.dir);
    AddMeshEntity(scene_, "rock", std::move(mesh), mat);
    return bounds;
  }

  // Building: placed at the origin, untransformed, so AddBuildingToScene's
  // returned LOCAL bounds are its world bounds -- used to frame the orbit.
  return AddBuildingToScene(scene_, matlib_, entry.building_kind,
                            glm::vec2(0.0f), /*yaw_radians=*/0.0f);
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

  // NOTE(lighting): every frame the editor mutates env_, ApplyEnvironment
  // re-derives the full sky (6 faces x face x face radiance), a 2048-sample SH
  // projection, and a GPU cube rebuild + IBL re-prefilter next frame -- fine
  // for occasional edits, but to be debounced / made incremental in the
  // future lighting commit. No behavior change here.
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
