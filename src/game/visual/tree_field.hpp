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
#include "game/geometry/tree_options.hpp"

namespace badlands {

class GpuPipelineGenerator;

// Owns an InstancedMeshField configured for ONE tree model (model id 0),
// LOD 0..GpuInstanceRenderer::kMaxLods-1, 2 submeshes per LOD (0 = bark
// kDeferred, 1 = leaf voxel crown kDeferred, skipped for a LOD whose supplied
// leaf mesh is empty -- see BuildTreeField below). Both submeshes cast
// shadows (Phase 5).
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
  std::array<LodBuffers, GpuInstanceRenderer::kMaxLods> lod_buffers;

  // One-time 1x1 support textures for the solid-color bark material (flat
  // tangent-space normal + a fixed-roughness ARM), kept alive the same way.
  wgpu::TextureView bark_normal_view;
  wgpu::TextureView bark_arm_view;
  wgpu::Sampler bark_support_sampler;

  // Raw (untransformed, native tree-generator units) LOD0 local-space
  // bounds -- the SAME bark_local_bounds a single-tree preview uses to
  // derive its scale+rest-on-floor transform (see
  // ModelViewerView::RebuildScene's single-tree path), plus the UNION of
  // every supplied leaf-crown LOD mesh's own bounds (voxel tets overscale
  // past their source cards' AABB, and different LODs can have different
  // extents -- see the .cpp) when the tree has any. A caller building
  // per-instance transforms combines these (bark_local_bounds.Union(
  // leaf_local_bounds) when has_leaves) and transforms the result by each
  // instance's FULL transform to get that instance's world-space bounds for
  // GpuInstanceRenderer::InstanceInput::bounds_sphere.
  Aabb bark_local_bounds;
  Aabb leaf_local_bounds;
  bool has_leaves = false;
};

// Builds a TreeField for `options`: the branch skeleton is built ONCE, then
// per LOD (0 = full detail; 1/2 = meshopt-simplified per kDefaultLodRatios,
// mesh_lod.hpp) a bark mesh is generated, and `leaf_lod_meshes[lod]` -- the
// caller's own pre-voxelized leaf-crown mesh for that LOD (see
// game/geometry/leaf_voxelizer.hpp's VoxelizeLeafCards; ModelViewerView's
// Multi mode voxelizes at kFoliageVoxelWorldSizes[lod] converted to native
// units) -- is taken as-is. Both are GPU-uploaded and wired into a fresh
// InstancedMeshField (capacity/lod_thresholds forwarded verbatim to its
// ctor; num_models=1, num_submeshes=2); both submeshes render kDeferred and
// cast shadows (see instanced_mesh_field.hpp's SetSubmeshShadow). A LOD
// whose supplied leaf mesh is empty (e.g. a coarse voxel cell size that
// clears no cell's occupancy threshold -- a known gap for some presets, see
// leaf_voxelizer.hpp) simply leaves that LOD's leaf submesh slot
// unconfigured (legal -- GpuInstanceRenderer::Draw skips zero-index-count
// slots); it does not affect the bark submesh or any other LOD. Returns
// nullptr (after logging) on any factory-build, field-compile, or
// bark-mesh failure, or if `leaf_lod_meshes.size() !=
// GpuInstanceRenderer::kMaxLods`.
std::unique_ptr<TreeField> BuildTreeField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    const TreeOptions& options,
    std::span<const TexturedMeshResult> leaf_lod_meshes, uint32_t capacity,
    std::array<float, GpuInstanceRenderer::kMaxLods - 1> lod_thresholds);

}  // namespace badlands
