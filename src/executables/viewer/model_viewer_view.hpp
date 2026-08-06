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
#include <span>
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
#include "game/geometry/tree_options.hpp"
#include "game/visual/impostor_baker.hpp"       // ImpostorBakeResult, BakeImpostorAtlas
#include "game/visual/instanced_lod_field.hpp"  // InstancedLodField, BuildInstancedLodField
#include "game/visual/prop_lod_model.hpp"       // BuildPropLodModel
#include "game/visual/tree_lod_model.hpp"       // BuildTreeFieldModel

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
    // Only floored, NOT clamped to kMultiLodLevel: that constant is the TREE's
    // maximum, and a prop's chain length is derived per model (see
    // lod_screen_space.hpp). The shipped props top out at Multi == 5, exactly
    // at the tree's cap with no margin, so a denser prop's Multi level would
    // silently become its impostor in a headless screenshot. RebuildScene
    // clamps against the selected generator's own maximum instead.
    lod_level_ = std::max(lod, 0);
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
  //   - `usdc_path`: an imported prop, shown through its own LOD chain.
  struct MeshGenerator {
    std::string name;
    std::function<GeneratedMesh()> generate;
    DeferredMaterial material;
    std::optional<TreeOptions> tree;
    // Set for imported props instead of `material`: the pack directory whose
    // material.json backs this entry, resolved in RebuildScene rather than
    // here. Resolving all of them up front would decode and upload every
    // prop's three textures just to show one, and -- worse -- MaterialLibrary's
    // load_failed_ flag is sticky and gates Initialize(), so a single prop with
    // a bad manifest would stop the viewer from starting at all, sphere and
    // trees included.
    std::string pack_dir;
    // The prop's source file, kept so the LOD path can re-import it without
    // going through `generate` (which returns one merged, already-transformed
    // mesh rather than the chain). Empty for non-prop entries.
    std::string usdc_path;
  };

  void BuildGenerators();
  // The imported prop's LOD chain for `generator_index_`, built on first use
  // and cached: parsing a .usdc plus welding and decimating it is ~a second,
  // and RebuildScene runs on every LOD radio click. Null for a non-prop
  // generator or if the import produced nothing.
  const InstancedLodModel* EnsurePropModel();
  // Highest valid lod_level_ for the current generator. Per-generator because
  // a prop's chain length is derived from its own size and triangle count, so
  // unlike the tree's fixed ladder it is not a constant -- see
  // game/visual/lod_screen_space.hpp.
  int MaxLodLevel();
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
  // Triangle count of the prop level currently shown; 0 in Multi mode, where
  // the GPU picks the level per instance.
  int prop_tris_ = 0;

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

  // Deferred alpha-cutout material for the "Original" leaf cards. Game-side
  // (shaders/game/foliage_cutout.wesl) rather than a MaterialLibrary entry: it
  // shares voxel_foliage's shading model and per-object UBO so the finest level
  // of the chain is lit identically to every coarser one, which is the whole
  // point of moving it off the forward path.
  std::unique_ptr<MaterialInstanceFactory> leaf_cutout_factory_;

  // The impostor preview, which now occupies the slot the retired voxel L3 used
  // to hold. Baked lazily per generator (the atlas is per MODEL, and the viewer
  // shows one at a time) and kept alive because the material only references
  // its views.
  std::unique_ptr<MaterialInstanceFactory> impostor_preview_factory_;
  ImpostorBakeResult impostor_preview_;
  int impostor_preview_generator_ = -1;

  // Bakes `models` into impostor_preview_ unless the current generator's atlas
  // is already there. Shared by BOTH impostor consumers -- the single-tree
  // "Impostor" level and Multi's LOD4 -- because they bake the same model at
  // the same preview height, so a second bake would be pure duplicate work.
  // Returns false if the bake failed; the caller then falls back to the
  // voxel-only chain.
  bool EnsureImpostorPreview(std::span<const InstancedLodModel> models);

  // Whether impostor_preview_ already holds the current generator's atlas.
  // Exposed so a caller can skip building the InstancedLodModel that
  // EnsureImpostorPreview would otherwise be handed and discard.
  bool ImpostorPreviewIsCurrent() const;

  // Builds the non-instanced foliage_impostor factory on first use. Shared by
  // the tree and prop single-model impostor previews -- the shader reads the
  // atlas, which neither model type varies.
  bool EnsureImpostorPreviewFactory();

  // See EnsurePropModel. `generator` is the index it was built for, so a
  // generator change invalidates it without a separate dirty flag.
  struct PropPreview {
    int generator = -1;
    InstancedLodModel model;
  };
  PropPreview prop_preview_;

  // GPU pipeline generator, stashed from Initialize()'s RenderContext --
  // BuildInstancedLodField (called from RebuildScene, not Initialize, since it needs
  // the currently-selected TreeOptions) needs it to build the instanced
  // material factories. Not owned; outlives this view (see render_context.hpp).
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  // Multi-mode instanced tree grid. Built in RebuildScene when lod_level_==4,
  // reset (and scene_context_.instanced_field_count cleared) otherwise.
  // field_ptr_ is the stable single-element array scene_context_.
  // instanced_fields points at (SceneContext::instanced_fields is
  // InstancedMeshField* const*, an array of field pointers).
  std::unique_ptr<InstancedLodField> tree_field_;
  InstancedMeshField* field_ptr_ = nullptr;

  ShadowDebugMode initial_shadow_debug_mode_ = ShadowDebugMode::Off;

  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
