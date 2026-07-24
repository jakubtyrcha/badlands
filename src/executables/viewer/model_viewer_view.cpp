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
  // Solid dark-brown bark color for the catalog tree meshes.
  bark_mat_ = matlib_.SolidColor(glm::vec3(0.30f, 0.19f, 0.10f), 0.9f);

  // Leaf-card silhouette texture, built once and shared by every tree. White
  // RGB (so the AlphaCutout material's per-tree tint colours it), alpha = leaf
  // shape. Uploaded with a full mip chain so distant cards antialias.
  {
    constexpr int kLeafTexSize = 64;
    std::vector<uint8_t> px = BuildLeafRgba8(kLeafTexSize, glm::vec3(1.0f));
    LoadedTexture leaf = UploadTexture2DWithMips(
        device_, queue_, *ctx.pipeline_gen, kLeafTexSize, kLeafTexSize,
        px.data());
    leaf_texture_ = leaf.texture;
    leaf_view_ = leaf.view;
    if (!leaf_view_) {
      spdlog::error("ModelViewerView::Initialize: leaf texture upload failed");
      return false;
    }
    // Trilinear + repeat sampler: the alpha mip chain must be sampled through a
    // Linear mipmapFilter (the material factory's default is Nearest, which
    // would defeat the mips and leave the edges aliased).
    wgpu::SamplerDescriptor samp = {};
    samp.minFilter = wgpu::FilterMode::Linear;
    samp.magFilter = wgpu::FilterMode::Linear;
    samp.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    samp.addressModeU = wgpu::AddressMode::Repeat;
    samp.addressModeV = wgpu::AddressMode::Repeat;
    samp.maxAnisotropy = 16;
    leaf_sampler_ = device_.CreateSampler(&samp);
  }

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
  // One entry per predefined tree setup (the full ez-tree preset catalog).
  // Trees go through the two-material path in RebuildScene (deferred bark +
  // forward-opaque alpha-cutout leaves), so they carry `tree` options rather
  // than a single-mesh `generate` lambda.
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

  const MeshGenerator& gen = generators_[generator_index_];

  // Frame on the WORLD-space bounds so the orbit centers on the object as it sits
  // on the floor.
  Aabb world_bounds = Aabb::Empty();
  if (gen.tree) {
    // Two-material tree: deferred solid bark + forward-opaque alpha-cutout leaf
    // cards, sharing the tree's local space (and therefore one preview
    // transform, so the leaves stay attached to the branches).
    TexturedMeshResult bark = GenerateTreeMesh(*gen.tree);
    const float h = bark.local_bounds.max.y - bark.local_bounds.min.y;
    const float s = kTreePreviewHeight / std::max(h, 0.001f);
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f),
                       glm::vec3(0.0f, -bark.local_bounds.min.y * s, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(s));

    world_bounds = bark.local_bounds.TransformedBy(xf);
    AddMeshEntity(scene_, "bark", std::move(bark), bark_mat_, xf);

    TexturedMeshResult leaves = GenerateLeafMesh(*gen.tree);
    if (leaves.mesh.vertex_count > 0) {
      world_bounds = world_bounds.Union(leaves.local_bounds.TransformedBy(xf));
      DeferredMaterial lm = matlib_.AlphaCutout(
          leaf_view_, leaf_sampler_, gen.tree->leaves.alpha_cutoff,
          gen.tree->leaves.tint);
      AddForwardOpaqueMeshEntity(scene_, "leaves", std::move(leaves),
                                 lm.factory, lm.params, xf);
    }
  } else if (gen.generate) {
    GeneratedMesh generated = gen.generate();
    world_bounds = generated.mesh.local_bounds.TransformedBy(generated.transform);
    AddMeshEntity(scene_, "mesh", std::move(generated.mesh), gen.material,
                  generated.transform);
  } else {
    // A MeshGenerator must set exactly one of `tree`/`generate`. Guard the
    // invariant so a malformed entry logs instead of throwing / rendering empty.
    spdlog::error("ModelViewerView::RebuildScene: generator '{}' has no mesh "
                  "generator; showing floor only",
                  gen.name);
    return;  // floor-only scene; leave the orbit framing unchanged
  }

  const glm::vec3 center = world_bounds.Center();
  const float radius = glm::length(world_bounds.max - center);
  orbit_.FrameBounds(center, radius > 0.01f ? radius : 1.0f);
  orbit_.UpdateCamera(camera_);
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
