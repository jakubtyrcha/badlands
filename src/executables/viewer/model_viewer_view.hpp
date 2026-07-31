#pragma once

// badlands_viewer's AppView: a procedural-mesh scaffold. An orbit camera around
// a single mesh produced by the selected generator, on a neutral gray floor,
// textured with a UV-checker debug material, lit by the simple LightEnvironment
// sun. generators_ is the extension point where future foliage/rock generators
// slot in. Lives in src/executables/viewer/ (an app, not the engine).

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // TexturedMeshResult
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_renderer.hpp"  // ShadowDebugMode
#include "game/visual/foliage_voxel_config.hpp"  // kFoliageVoxelWorldSizes
#include "engine/scene/scene_graph.hpp"
#include "game/geometry/leaf_texture.hpp"
#include "game/geometry/tree_options.hpp"  // TreeOptions
#include "game/visual/tree_field.hpp"       // TreeField, BuildTreeField

namespace badlands {

class ModelViewerView : public AppView {
 public:
  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

  // Selects the generator shown once Initialize() builds the registry + scene.
  // Must be called before Initialize() -- main_viewer.cpp's `--generator <n>`
  // CLI arg uses it for headless screenshot verification. Out-of-range indices
  // are clamped in Initialize().
  void SetInitialGeneratorIndex(int index) { generator_index_ = index; }

  // Selects the initial ShadowDebugMode (headless `--shadow-debug <n>`:
  // 0=Off, 1=Combined, 2=ShadowMapOnly, 3=ContactOnly). Call before Initialize().
  void SetInitialShadowDebugMode(ShadowDebugMode mode) {
    initial_shadow_debug_mode_ = mode;
  }

  // The lod_level_ layout, exposed so the CLI clamps against the same bounds
  // the view switches on (a stale hardcoded clamp here silently pinned every
  // `--lod` past the old maximum to the last voxel level). 0 = "Original";
  // 1..kVoxelLodCount = "Voxel L0..L3", one per kFoliageVoxelWorldSizes
  // entry; kMultiLodLevel = "Multi". Adding a voxel level means adding a cell
  // size to that array (plus its bark budget -- see
  // kFoliageCoarseBarkTriBudgets' static_assert); nothing here or in
  // main_viewer.cpp changes.
  static constexpr int kVoxelLodCount =
      static_cast<int>(kFoliageVoxelWorldSizes.size());
  static constexpr int kMultiLodLevel = kVoxelLodCount + 1;

  // Selects the initial LOD level (headless `--lod <n>`, 0..kMultiLodLevel):
  // 0 = "Original" (today's full bark + billboard-card leaves),
  // 1..kVoxelLodCount = "Voxel L0..L3" (tet-voxelized crowns at progressively
  // coarser cell sizes), kMultiLodLevel = "Multi" (a 16x16 instanced grid of
  // the selected tree with dynamic GPU LOD). Call before Initialize() --
  // RebuildScene() reads lod_level_ when generating tree meshes.
  void SetInitialLod(int lod) {
    lod_level_ = std::clamp(lod, 0, kMultiLodLevel);
  }

 private:
  // The output of a generator: a mesh plus the transform that places it. The
  // generator assumes the floor is at y=0 and returns the resting offset as a
  // transform -- it does NOT bake the offset into the mesh vertices, so the
  // mesh stays in its own natural local space.
  struct GeneratedMesh {
    TexturedMeshResult mesh;
    glm::mat4 transform{1.0f};
  };
  // A named entry in the viewer's list. Exactly one path is set:
  //   - `generate`: a single-material mesh entity (the sphere test object).
  //   - `tree`: a catalog tree, built in RebuildScene as TWO materials --
  //     deferred solid bark + forward-opaque alpha-cutout leaf cards.
  struct MeshGenerator {
    std::string name;
    std::function<GeneratedMesh()> generate;
    DeferredMaterial material;
    std::optional<TreeOptions> tree;
  };

  void BuildGenerators();
  // Re-derives env_'s sky/SH/sun into scene_context_ and mirrors it into scene_.
  void ApplyEnvironment();
  // Fresh graph: re-mirror lighting, add the gray floor at y=0, then add the
  // selected mesh generator's entity. Reframes the orbit.
  void RebuildScene();
  // Returns the cached per-silhouette leaf-card view (see leaf_views_ below).
  wgpu::TextureView LeafViewFor(LeafSilhouette shape) const {
    return leaf_views_[static_cast<size_t>(shape)];
  }

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  OrbitCameraController orbit_;

  std::vector<MeshGenerator> generators_;
  int generator_index_ = 0;

  // Manual LOD switch (tree generators only): 0="Original" (full-detail bark
  // + billboard-card leaves via TranslucentFoliage, byte-for-byte the old
  // lod-0 path), 1..4="Voxel L0..L3" (bark simplified per
  // SimplifyBarkForVoxelLod, leaves replaced by a tet-voxelized crown via
  // VoxelizeLeafCards/VoxelFoliage at progressively coarser cell sizes --
  // see kFoliageVoxelWorldSizes), 5="Multi" (a 16x16 instanced grid via
  // tree_field_, dynamic
  // GPU LOD -- see RebuildScene; voxel crowns since Phase 5, not card
  // leaves). The bounds come from kFoliageVoxelWorldSizes' size, not
  // hardcoded here -- see model_viewer_view.cpp's kVoxelLodCount /
  // kMultiLodLevel. bark_tris_/leaf_tris_ are the single-tree triangle
  // counts, recomputed in RebuildScene for the ImGui readout; not meaningful
  // in Multi mode.
  int lod_level_ = 0;
  int bark_tris_ = 0;
  int leaf_tris_ = 0;

  DeferredMaterial checker_mat_;  // UV-checker debug material for the sphere
  DeferredMaterial bark_mat_;     // Solid bark color for catalog tree meshes

  // Per-silhouette leaf-card textures: white RGB (alpha = leaf shape), one CPU
  // mip chain per LeafSilhouette built once in Initialize and shared by every
  // tree of that silhouette, coloured per-tree via the AlphaCutout/foliage
  // material tint. Indexed by static_cast<size_t>(LeafSilhouette) (see
  // LeafViewFor above); leaf_textures_ owns the GPU textures (leaf_views_
  // alone doesn't keep them alive). leaf_sampler_ is one shared trilinear +
  // clamp-to-edge + aniso-16 sampler so every silhouette's mip chain is
  // sampled the same way. Sized via tree_options.hpp's canonical
  // kLeafSilhouetteCount, not a hand-copied literal.
  std::array<wgpu::Texture, kLeafSilhouetteCount> leaf_textures_;
  std::array<wgpu::TextureView, kLeafSilhouetteCount> leaf_views_;
  wgpu::Sampler leaf_sampler_;

  // GPU pipeline generator, stashed from Initialize()'s RenderContext --
  // BuildTreeField (called from RebuildScene, not Initialize, since it needs
  // the currently-selected TreeOptions) needs it to build the instanced
  // material factories. Not owned; outlives this view (see render_context.hpp).
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  // Multi-mode instanced tree grid. Built in RebuildScene when lod_level_==4,
  // reset (and scene_context_.instanced_field_count cleared) otherwise.
  // field_ptr_ is the stable single-element array scene_context_.
  // instanced_fields points at (SceneContext::instanced_fields is
  // InstancedMeshField* const*, an array of field pointers).
  std::unique_ptr<TreeField> tree_field_;
  InstancedMeshField* field_ptr_ = nullptr;

  ShadowDebugMode initial_shadow_debug_mode_ = ShadowDebugMode::Off;

  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
