#pragma once

// Game-side builder for an InstancedMeshField that renders a whole catalog
// tree as ONE GPU-instanced model (bark = kDeferred submesh 0, leaf voxel
// crown = kDeferred submesh 1, BOTH shadow-casting -- volumetric-foliage
// Phase 5) with GPU-driven distance LOD (see
// engine/rendering/instanced_mesh_field.hpp). Used by the model viewer's
// "Multi" mode (a grid of one tree, dynamically LOD'd) -- see
// executables/viewer/model_viewer_view.cpp.
//
// TreeField owns everything the field needs: the two instanced material
// factories (instanced_gbuffer for bark, voxel_foliage for the leaf voxel
// crown), a local MaterialInstanceCache + the resource handles that keep the
// resolved materials alive (mirrors gpu_instance_tests.cpp's BucketMaterials
// pattern), and the per-LOD GPU vertex/index buffers (SetSubmesh does not
// take ownership of these -- see instanced_mesh_field.hpp).
//
// Deviation from a literal reading of the task brief: `instanced_gbuffer`
// has NO entry in engine/rendering/material/material_requirements.cpp
// (unlike "normalmapped"/"standard_forward"), so StandardMaterialFactory
// falls back to DeriveRequirementsFromReflection, which names group-0
// texture slots "tex_<binding>" (not "albedo"/"normal"/"arm") and always
// defaults them to "white". BuildTreeField below targets those
// reflection-derived names directly (see the per-shader binding-index
// comments in the .cpp) for the BARK material's normal/ARM support textures,
// rather than "albedo"/"normal"/"arm" -- using the literal names would
// silently no-op the override, leaving the flat reflection-derived default
// instead of the intended solid-bark look. This stays entirely within this
// game-side file; no engine file is touched. (The leaf material,
// `voxel_foliage`, declares NO textures at all -- see
// shaders/material/voxel_foliage.wesl -- so this deviation no longer applies
// to leaves as of volumetric-foliage Phase 5; the leaf card + its "tex_1"
// albedo override are gone.)

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // TexturedMeshResult
#include "engine/rendering/gpu_instance_renderer.hpp"
#include "engine/rendering/instanced_mesh_field.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "game/geometry/tree_generator.hpp"  // SkeletonBranch
#include "game/geometry/tree_options.hpp"
#include "game/visual/impostor_atlas.hpp"

namespace badlands {

class GpuPipelineGenerator;

// The solid colour every tree's bark renders with. Named and shared because the
// impostor bake (game/visual/impostor_baker.hpp) has to reproduce it exactly --
// if the two drift, a tree visibly changes hue at the LOD4 switch distance,
// which reads as a lighting bug rather than as a mismatched constant.
inline constexpr glm::vec3 kTreeBarkColor{0.30f, 0.19f, 0.10f};

// One model's fully-prepared CPU geometry. The caller builds these (see
// BuildTreeFieldModel below, which is the standard way) and BuildTreeField
// uploads them -- generation stays on the caller's side so a caller that
// already needed the bark mesh for something else does not pay for a second,
// byte-identical generation pass.
struct TreeFieldModel {
  // Only the leaf tint/transmission are read from this at build time; it is
  // kept whole so the model stays self-describing.
  TreeOptions options;
  TexturedMeshResult bark_lod0;
  // One pre-voxelized crown per LOD. An entry may legitimately be EMPTY (see
  // leaf_voxelizer.hpp's known gap) -- that LOD then renders bare rather than
  // failing the build.
  std::vector<TexturedMeshResult> leaf_lod_meshes;
  // Ascending distance cutoffs, exactly leaf_lod_meshes.size() - 1 of them.
  std::vector<float> lod_thresholds;
  // Native ez-tree units -> world metres for this model, i.e.
  // target_height_m / bark height in native units. The caller needs it to build
  // instance transforms, so it is computed once here rather than re-derived.
  float native_to_world_scale = 1.0f;
  // What this model was built to stand at, in metres. Derivable from the bark
  // height and the scale above, but stored because the LOD chain's cutoffs are
  // a function of it and re-deriving them is how the two drift.
  float target_height_m = 1.0f;
};

// The optional impostor level (LOD4). Supplying a valid atlas whose placement
// count matches the models adds one LOD to every model; leaving it default
// builds exactly the voxel-only chain that existed before.
struct TreeFieldImpostor {
  const ImpostorAtlas* atlas = nullptr;
  std::span<const ImpostorPlacement> placement;

  bool active(size_t model_count) const {
    return atlas != nullptr && atlas->valid() &&
           placement.size() == model_count;
  }
};

// Per-model local-space bounds, in NATIVE (untransformed) units.
struct TreeModelBounds {
  Aabb bark_local_bounds = Aabb::Empty();
  // UNION over every supplied crown LOD: voxel tets overscale past their source
  // cards' AABB, and different LODs have different extents, so no single LOD's
  // bounds are conservative for all of them.
  Aabb leaf_local_bounds = Aabb::Empty();
  bool has_leaves = false;

  // What a caller should transform by an instance's full matrix to get that
  // instance's world bounds for InstanceInput::bounds_sphere.
  Aabb Combined() const {
    return has_leaves ? bark_local_bounds.Union(leaf_local_bounds)
                      : bark_local_bounds;
  }
};

// Owns an InstancedMeshField configured for N tree models, each with its OWN
// runtime LOD count and its own distance cutoffs (GpuInstanceRenderer::ModelLod
// is per model), bounded by the engine's compile-time kMaxLods cap. 2 submeshes
// per (model, LOD): 0 = bark kDeferred, 1 = leaf voxel crown kDeferred, the
// latter skipped for any (model, LOD) whose supplied leaf mesh is empty. Both
// submeshes cast shadows (Phase 5).
//
// A single-tree caller passes a 1-element span and gets exactly the previous
// behaviour; a forest passes ~28 (see game/visual/forest_catalog.hpp).
struct TreeField {
  std::unique_ptr<InstancedMeshField> field;

  std::unique_ptr<MaterialInstanceFactory> bark_factory;
  std::unique_ptr<MaterialInstanceFactory> leaf_factory;
  MaterialInstanceCache material_cache;
  // Keeps every resolved material instance alive for as long as this
  // TreeField lives (InstancedMeshField::SetSubmesh's `material` is not
  // owned -- see instanced_mesh_field.hpp).
  std::vector<entt::resource<RenderingMaterialInstance>> material_handles;

  // Per-LOD GPU buffers, kept alive for the same reason (not owned by
  // InstancedMeshField/GpuInstanceRenderer).
  struct LodBuffers {
    wgpu::Buffer bark_vertex_buffer;
    wgpu::Buffer bark_index_buffer;
    wgpu::Buffer leaf_vertex_buffer;
    wgpu::Buffer leaf_index_buffer;
  };
  // Indexed [model][lod]. The inner vector holds one entry per LOD that model
  // was actually built with (its runtime LOD count), NOT kMaxLods entries --
  // that constant is only the cap.
  std::vector<std::vector<LodBuffers>> lod_buffers;

  // The impostor level's shared unit quad (one for every model -- the geometry
  // is identical, only the material differs) and its factory. Null unless a
  // TreeFieldImpostor was supplied.
  std::unique_ptr<MaterialInstanceFactory> impostor_factory;
  wgpu::Buffer impostor_vertex_buffer;
  wgpu::Buffer impostor_index_buffer;

  // One-time 1x1 support textures for the solid-color bark material (flat
  // tangent-space normal + a fixed-roughness ARM), kept alive the same way.
  wgpu::TextureView bark_normal_view;
  wgpu::TextureView bark_arm_view;
  wgpu::Sampler bark_support_sampler;

  // Per-model raw (untransformed, native tree-generator units) local-space
  // bounds -- for model 0 these are the SAME bark_local_bounds a single-tree
  // preview uses to derive its scale + rest-on-floor transform (see
  // ModelViewerView::RebuildScene's single-tree path). Parallel to the
  // `models` span BuildTreeField was given.
  std::vector<TreeModelBounds> model_bounds;

  // Native -> world scale per model, copied from TreeFieldModel so a caller
  // building instance transforms does not have to keep the inputs alive.
  std::vector<float> native_to_world_scale;

  size_t model_count() const { return model_bounds.size(); }
};

// Prepares ONE model's CPU geometry for a tree that stands `target_height_m`
// tall in the world: skeleton, LOD0 bark, and one voxelized crown per level of
// the standard foliage chain (kFoliageVoxelWorldSizes), with the cell sizes and
// the distance cutoffs both retargeted from the 8 m preview tree to this one --
// see FoliageLodThresholdsForHeight in foliage_voxel_config.hpp for why both
// scale by the same ratio.
//
// Pure CPU and independent per model, so a caller building a whole forest can
// run these in parallel and upload the results serially.
TreeFieldModel BuildTreeFieldModel(const TreeOptions& options,
                                   float target_height_m);

// Builds a TreeField from N already-prepared models (BuildTreeFieldModel is the
// standard way to get them). Generation stays on the caller's side on purpose:
// this function used to regenerate the skeleton and bark internally, a second
// byte-identical pass on top of whatever the caller had already built them for.
//
// Each model contributes its own GpuInstanceRenderer::ModelLod chain (its LOD
// count is its `leaf_lod_meshes.size()`, its cutoffs its `lod_thresholds`), so
// models with different LOD counts coexist in one field. Per (model, LOD): the
// bark mesh is LOD0 decimated by the shared SimplifyBarkForVoxelLod policy, and
// the crown is `leaf_lod_meshes[lod]` taken as-is. The field is created with
// num_models = models.size() and num_submeshes = 2; both submeshes render
// kDeferred and cast shadows (see instanced_mesh_field.hpp's SetSubmeshShadow).
//
// A (model, LOD) whose supplied leaf mesh is empty -- a coarse voxel cell size
// that clears no cell's occupancy threshold, a known gap for some presets (see
// leaf_voxelizer.hpp) -- simply leaves that leaf submesh slot unconfigured,
// which is legal: GpuInstanceRenderer::Draw skips zero-index-count slots. It
// affects neither that LOD's bark nor any other LOD or model.
//
// Returns nullptr (after logging) on any factory-build, field-compile or
// bark-mesh failure, if `models` is empty, or if any model's LOD count is
// outside [1, GpuInstanceRenderer::kMaxLods] or its `lod_thresholds.size() !=
// lod_count - 1` (one ascending cutoff between each adjacent pair of levels).
std::unique_ptr<TreeField> BuildTreeField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    std::span<const TreeFieldModel> models, uint32_t capacity,
    const TreeFieldImpostor& impostor = {});

}  // namespace badlands
