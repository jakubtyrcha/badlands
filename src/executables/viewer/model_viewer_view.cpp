#include "executables/viewer/model_viewer_view.hpp"

#include <algorithm>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>  // glm::translate
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/sdl_input_util.hpp"  // NormalizedWheelY
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/tree_options.hpp"

namespace badlands {

namespace {

// Flat mid-gray debug floor. Kept dark enough that the sun + sky ambient don't
// clip it to pure white (which washed out thin bark tubes) -- a mid-gray floor
// silhouettes the generated mesh with contrast. Roughness maxed to keep it
// diffuse so shadows read clearly.
constexpr glm::vec3 kFloorGray{0.5f};
constexpr float kFloorRoughness = 1.0f;
constexpr float kFloorSize = 40.0f;
// One floor-UV repeat per ~2 world units instead of stretching one copy.
constexpr float kFloorUvRepeatSpacing = 2.0f;
// Preview height the tree generators are display-scaled to (their native ez-tree
// units are tens-of-meters tall, which frames far away and reads tiny).
constexpr float kTreePreviewHeight = 8.0f;

}  // namespace

bool ModelViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("ModelViewerView::Initialize: MaterialLibrary init failed");
    return false;
  }

  // UV-checker debug material (two distinct grays) for the sphere test object, so
  // its UVs read against the flat gray floor.
  checker_mat_ = matlib_.CheckerAlbedo(glm::vec3(0.85f), glm::vec3(0.35f));

  BuildGenerators();
  if (generators_.empty()) {
    spdlog::error("ModelViewerView::Initialize: empty generator registry");
    return false;
  }
  generator_index_ =
      std::clamp(generator_index_, 0, static_cast<int>(generators_.size()) - 1);

  // No volumetric fog in the model viewer -- it renders soft media blobs behind
  // the mesh that only make sense in the game world.
  scene_renderer_->MutableFogConfig().enabled = false;

  // The default sun+sky (intensity 3.0 / 1.0) overexposes the scene and washes
  // out thin bark tubes. Dial both back so the floor lands mid-gray and the
  // generated mesh reads with contrast.
  env_.sun_intensity = 2.0f;
  env_.sky_intensity = 0.5f;

  ApplyEnvironment();
  RebuildScene();
  scene_renderer_->SetShadowDebugMode(initial_shadow_debug_mode_);

  if (!matlib_.ok()) {
    spdlog::error("ModelViewerView::Initialize: material load failed");
    return false;
  }
  return true;
}

void ModelViewerView::BuildGenerators() {
  generators_.clear();
  // The "test" generator: the engine's cube-sphere (cube -> 16x16 per face ->
  // normalized sphere, EAC UVs). Future foliage/rock generators append here.
  generators_.push_back(
      {.name = "Sphere (test)", .generate = [] {
         TexturedMeshResult mesh = GenerateSphereTexturedMesh(1.0f, 16);
         // Floor is at y=0: lift the mesh so its lowest point rests on it. The
         // offset is a transform, never baked into the vertices.
         const glm::mat4 transform = glm::translate(
             glm::mat4(1.0f), glm::vec3(0.0f, -mesh.local_bounds.min.y, 0.0f));
         return GeneratedMesh{std::move(mesh), transform};
       }, .material = checker_mat_});
  // One entry per predefined tree setup (the full ez-tree preset catalog). Each
  // is drawn as its skeleton graph (see RebuildScene), not a tube mesh.
  for (const NamedTreeOptions& setup : TreeCatalog()) {
    generators_.push_back({.name = setup.name, .tree = setup.options});
  }
}

void ModelViewerView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void ModelViewerView::RebuildScene() {
  // Fresh graph drops every prior entity; its ctor resets sun/ambient to
  // SceneGraph defaults, so re-mirror scene_context_'s derived lighting.
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor(scene_, kFloorSize, matlib_.SolidColor(kFloorGray, kFloorRoughness),
           kFloorSize / kFloorUvRepeatSpacing);

  tree_lines_.Clear();
  const MeshGenerator& gen = generators_[generator_index_];

  // Frame on the WORLD-space bounds so the orbit centers on the object as it sits
  // on the floor.
  Aabb world_bounds = Aabb::Empty();
  if (gen.tree.has_value()) {
    BuildTreeGraph(*gen.tree, world_bounds);
  } else {
    GeneratedMesh generated = gen.generate();
    world_bounds = generated.mesh.local_bounds.TransformedBy(generated.transform);
    AddMeshEntity(scene_, "mesh", std::move(generated.mesh), gen.material,
                  generated.transform);
  }

  const glm::vec3 center = world_bounds.Center();
  const float radius = glm::length(world_bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
}

void ModelViewerView::BuildTreeGraph(const TreeOptions& options,
                                     Aabb& out_world_bounds) {
  // Per-level line colors (trunk -> twig) and pixel thickness. The bare tube mesh
  // reads poorly at this scale, so the preview shows the branch GRAPH instead.
  static const glm::vec3 kLevelColor[4] = {
      {0.40f, 0.26f, 0.13f},  // trunk
      {0.55f, 0.36f, 0.16f},  // level 1
      {0.45f, 0.55f, 0.22f},  // level 2
      {0.60f, 0.70f, 0.32f},  // level 3
  };

  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(options);

  // Native bounds from the section origins, then a display scale to a consistent
  // preview height (ez-tree presets are tens of metres tall).
  Aabb native = Aabb::Empty();
  for (const SkeletonBranch& br : skeleton)
    for (const BranchSection& s : br.sections) {
      native.min = glm::min(native.min, s.origin);
      native.max = glm::max(native.max, s.origin);
    }
  const float height = native.max.y - native.min.y;
  const float scale = kTreePreviewHeight / std::max(height, 0.001f);
  const glm::mat4 xf = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

  for (const SkeletonBranch& br : skeleton) {
    const glm::vec3 color = kLevelColor[std::clamp(br.level, 0, 3)];
    const float thickness = std::max(1.0f, 5.0f - static_cast<float>(br.level) * 1.2f);
    for (size_t k = 1; k < br.sections.size(); ++k) {
      const glm::vec3 a = glm::vec3(xf * glm::vec4(br.sections[k - 1].origin, 1.0f));
      const glm::vec3 b = glm::vec3(xf * glm::vec4(br.sections[k].origin, 1.0f));
      tree_lines_.AddLine(a, b, color, thickness);
    }
  }

  out_world_bounds = native.TransformedBy(xf);
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
      orbit_.HandleMouseWheel(NormalizedWheelY(event.wheel));
      break;
    default:
      break;
  }
}

void ModelViewerView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
  // Point the renderer's debug-line pass at the current tree graph (if any).
  scene_context_.debug_lines = tree_lines_.empty() ? nullptr : &tree_lines_;
}

void ModelViewerView::DrawUI() {
  if (!scene_renderer_ || generators_.empty()) return;

  // Mesh-setup window: single-select generator list. Give it a sensible default
  // size and a minimum-size floor -- the list now holds the sphere + the full
  // tree catalog, and a previously-persisted tiny window (from when it held only
  // a few entries) would otherwise clip the list. The constraint clamps any
  // stale/tiny persisted size up every frame; the list scrolls if it overflows.
  int selected = generator_index_;
  ImGui::SetNextWindowSize(ImVec2(240.0f, 460.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 240.0f),
                                      ImVec2(4096.0f, 4096.0f));
  ImGui::Begin("Mesh");
  for (int i = 0; i < static_cast<int>(generators_.size()); ++i) {
    if (ImGui::Selectable(generators_[i].name.c_str(), i == generator_index_)) {
      selected = i;
    }
  }
  ImGui::End();

  if (selected != generator_index_) {
    generator_index_ = selected;
    RebuildScene();
  }

  // Visual-setup window: the shared rendering-debug + light editor ("Debug").
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
