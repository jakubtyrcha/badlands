#pragma once

// Game-side builder for an InstancedMeshField that renders a whole catalog
// tree as ONE GPU-instanced model (bark = kDeferred submesh 0, leaf card =
// kForwardOpaque submesh 1) with GPU-driven distance LOD (see
// engine/rendering/instanced_mesh_field.hpp). Used by the model viewer's
// "Multi" mode (a grid of one tree, dynamically LOD'd) -- see
// executables/viewer/model_viewer_view.cpp.
//
// TreeField owns everything the field needs: the two instanced material
// factories (instanced_gbuffer for bark, instanced_forward for leaves), a
// local MaterialInstanceCache + the resource handles that keep the resolved
// materials alive (mirrors gpu_instance_tests.cpp's BucketMaterials
// pattern), and the per-LOD GPU vertex/index buffers (SetSubmesh does not
// take ownership of these -- see instanced_mesh_field.hpp).
//
// Deviation from a literal reading of the task brief: `instanced_gbuffer`/
// `instanced_forward` have NO entry in
// engine/rendering/material/material_requirements.cpp (unlike
// "normalmapped"/"standard_forward"), so StandardMaterialFactory falls back
// to DeriveRequirementsFromReflection, which names group-0 texture slots
// "tex_<binding>" (not "albedo"/"normal"/"arm") and always defaults them to
// "white". BuildTreeField below targets those reflection-derived names
// directly (see the per-shader binding-index comments in the .cpp) rather
// than "albedo"/"normal"/"arm" -- using the literal names from the brief
// would silently no-op every texture override (e.g. the leaf cutout alpha
// would never discard, rendering solid quads instead of leaf cards). This
// stays entirely within this game-side file; no engine file is touched.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/rendering/geometry/aabb.hpp"
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
// kDeferred, 1 = leaf kForwardOpaque, skipped if the tree has no leaves).
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
  // ModelViewerView::RebuildScene's single-tree path), plus the leaf
  // card's own bounds when the tree has any. A caller building per-instance
  // transforms combines these (bark_local_bounds.Union(leaf_local_bounds)
  // when has_leaves) and transforms the result by each instance's FULL
  // transform to get that instance's world-space bounds for
  // GpuInstanceRenderer::InstanceInput::bounds_sphere.
  Aabb bark_local_bounds;
  Aabb leaf_local_bounds;
  bool has_leaves = false;
};

// Builds a TreeField for `options`: the branch skeleton is built ONCE, then
// per LOD (0 = full detail; 1/2 = meshopt-simplified, matching the viewer's
// manual-LOD kLodRatios) bark + leaf meshes are generated, GPU-uploaded, and
// wired into a fresh InstancedMeshField (capacity/lod_thresholds forwarded
// verbatim to its ctor; num_models=1, num_submeshes=2). `leaf_view`/
// `leaf_sampler` supply the leaf-card silhouette texture (see
// ModelViewerView::Initialize's leaf_view_/leaf_sampler_). Returns nullptr
// (after logging) on any factory-build, field-compile, or bark-mesh failure.
std::unique_ptr<TreeField> BuildTreeField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    const TreeOptions& options, wgpu::TextureView leaf_view,
    wgpu::Sampler leaf_sampler, uint32_t capacity,
    std::array<float, GpuInstanceRenderer::kMaxLods - 1> lod_thresholds);

}  // namespace badlands
