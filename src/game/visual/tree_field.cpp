#include "game/visual/tree_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // kTexturedMeshFloatsPerVertex
#include "engine/rendering/texture_loader.hpp"                   // CreateSolidColorTexture
#include "game/geometry/mesh_lod.hpp"
#include "game/geometry/tree_generator.hpp"

namespace badlands {

namespace {

// mesh_lod.hpp's kDefaultLodRatios sized to GpuInstanceRenderer::kMaxLods:
// one ratio per bark LOD bucket this field builds below. Leaves no longer go
// through this file's own LOD simplification (volumetric-foliage Phase 5 --
// the caller supplies one pre-voxelized mesh per LOD directly).
static_assert(kDefaultLodRatios.size() == GpuInstanceRenderer::kMaxLods);

// Matte roughness for the bark ARM texture AND the voxel-leaf material's
// `params.x` -- same rationale/value as model_viewer_view.cpp's single-tree
// bark_mat_ (SolidColor(..., 0.9f)) and MaterialLibrary's local
// kFoliageRoughness convention (material_library.cpp).
constexpr float kBarkRoughness = 0.9f;

// Write-once buffer: contents are fully supplied via mappedAtCreation, so no
// CopyDst (the buffer is never subsequently queue.WriteBuffer'd) and no
// wgpu::Queue parameter (the memcpy-into-mapped-range upload never touches
// the queue).
wgpu::Buffer UploadBuffer(wgpu::Device device, const void* data, uint64_t size,
                          wgpu::BufferUsage usage) {
  if (size == 0) return nullptr;
  wgpu::BufferDescriptor desc{};
  desc.size = size;
  desc.usage = usage;
  desc.mappedAtCreation = true;
  wgpu::Buffer buffer = device.CreateBuffer(&desc);
  if (!buffer) return nullptr;
  std::memcpy(buffer.GetMappedRange(0, size), data, size);
  buffer.Unmap();
  return buffer;
}

wgpu::Buffer UploadVertexBuffer(wgpu::Device device,
                                const std::vector<float>& vertices) {
  return UploadBuffer(device, vertices.data(), vertices.size() * sizeof(float),
                      wgpu::BufferUsage::Vertex);
}

wgpu::Buffer UploadIndexBuffer(wgpu::Device device,
                               const std::vector<uint32_t>& indices) {
  return UploadBuffer(device, indices.data(),
                      indices.size() * sizeof(uint32_t),
                      wgpu::BufferUsage::Index);
}

// Simplifies `mesh` in place to `ratio` (a no-op for ratio >= 1.0, since
// SimplifyMesh short-circuits that case -- so calling this with
// kDefaultLodRatios[0] == 1.0 would still leave the LOD0 mesh byte-identical
// to a freshly generated one).
void SimplifyInPlace(StaticTexturedMeshComponent& mesh, float ratio) {
  SimplifiedMesh simplified = SimplifyMesh(
      mesh.vertices, kTexturedMeshFloatsPerVertex, mesh.indices, ratio);
  mesh.vertices = std::move(simplified.vertices);
  mesh.indices = std::move(simplified.indices);
  mesh.vertex_count = simplified.vertex_count;
}

}  // namespace

std::unique_ptr<TreeField> BuildTreeField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    const TreeOptions& options,
    std::span<const TexturedMeshResult> leaf_lod_meshes, uint32_t capacity,
    std::array<float, GpuInstanceRenderer::kMaxLods - 1> lod_thresholds) {
  if (leaf_lod_meshes.size() != GpuInstanceRenderer::kMaxLods) {
    spdlog::error(
        "BuildTreeField: leaf_lod_meshes.size()={} != kMaxLods={}",
        leaf_lod_meshes.size(), GpuInstanceRenderer::kMaxLods);
    return nullptr;
  }

  auto tf = std::make_unique<TreeField>();

  // --- Bark factory: instanced_gbuffer / kDeferred, shadow-casting
  // (volumetric-foliage Phase 5 -- instanced_gbuffer.wesl already compiles a
  // shadow_pass variant; see the .hpp's deviation note for the shadow
  // instance's texture handling). ---
  {
    FactoryDescriptor desc;
    desc.shader_name = "instanced_gbuffer";
    desc.shader_path = "material/instanced_gbuffer";
    desc.supported_pass_types = {MaterialPassType::kDeferred};
    desc.supported_geometry_types = {GeometryType::kInstancedMesh};
    desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                          GBuffer::kMaterialFormat};
    desc.depth_format = GBuffer::kDepthFormat;
    desc.cull_mode = wgpu::CullMode::None;
    desc.casts_shadow = true;
    tf->bark_factory = BuildMaterialInstanceFactory(desc, device, queue,
                                                     &pipeline_gen);
    if (!tf->bark_factory) {
      spdlog::error(
          "BuildTreeField: failed to build instanced_gbuffer factory");
      return nullptr;
    }
  }

  // --- Leaf factory: voxel_foliage / kDeferred (volumetric-foliage Phase 5
  // -- replaces the old instanced_forward leaf-card path), textureless
  // (see shaders/material/voxel_foliage.wesl), Back-face culled (solid
  // tets, not double-sided cards), shadow-casting. Mirrors
  // MaterialLibrary::VoxelFoliage's descriptor (material_library.cpp) but on
  // kInstancedMesh geometry. ---
  {
    FactoryDescriptor desc;
    desc.shader_name = "voxel_foliage";
    desc.shader_path = "material/voxel_foliage";
    desc.supported_pass_types = {MaterialPassType::kDeferred};
    desc.supported_geometry_types = {GeometryType::kInstancedMesh};
    desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                          GBuffer::kMaterialFormat};
    desc.depth_format = GBuffer::kDepthFormat;
    desc.cull_mode = wgpu::CullMode::Back;
    desc.casts_shadow = true;
    tf->leaf_factory = BuildMaterialInstanceFactory(desc, device, queue,
                                                     &pipeline_gen);
    if (!tf->leaf_factory) {
      spdlog::error("BuildTreeField: failed to build voxel_foliage factory");
      return nullptr;
    }
  }

  tf->field = std::make_unique<InstancedMeshField>(
      device, queue, pipeline_gen, capacity, /*num_models=*/1u,
      /*num_submeshes=*/2u, lod_thresholds);
  if (!tf->field->IsValid()) {
    spdlog::error("BuildTreeField: InstancedMeshField compile failed");
    return nullptr;
  }

  // One-time bark support textures: instanced_gbuffer has no roughness/tint
  // uniform for bark beyond `tint` (see the .hpp's deviation note) -- a flat
  // tangent-space normal (128,128,255) and a fixed-roughness ARM
  // (255, kBarkRoughness*255, 0) reproduce the single-tree
  // MaterialLibrary::SolidColor look (material_library.cpp:82-116) for the
  // instanced bark material.
  tf->bark_normal_view =
      CreateSolidColorTexture(device, queue, 128, 128, 255, 255);
  const uint8_t rough_byte = static_cast<uint8_t>(
      std::lround(std::clamp(kBarkRoughness, 0.0f, 1.0f) * 255.0f));
  tf->bark_arm_view =
      CreateSolidColorTexture(device, queue, 255, rough_byte, 0, 255);
  wgpu::SamplerDescriptor support_sampler_desc{};
  tf->bark_support_sampler = device.CreateSampler(&support_sampler_desc);

  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(options);

  // Generate the bark mesh ONCE at LOD0 -- generation is deterministic, so a
  // fresh GenerateTreeMesh call per lod (as this used to do) would reproduce
  // byte-identical vertices/indices at 3x the cost. LOD1/2 below simplify a
  // COPY of this LOD0 data at the shared ratio (kDefaultLodRatios), the
  // single-tree path's exact pattern (model_viewer_view.cpp). Leaves have no
  // such step here anymore: `leaf_lod_meshes[lod]` is already the caller's
  // final per-LOD mesh (volumetric-foliage Phase 5).
  TexturedMeshResult bark_lod0 = GenerateTreeMesh(options, skeleton);
  tf->bark_local_bounds = bark_lod0.local_bounds;

  // leaf_local_bounds = the UNION of every supplied LOD's own bounds (voxel
  // tets overscale past their source cards' AABB, and different LODs can
  // have different extents, so no single LOD's bounds alone would be
  // conservative for every LOD). Aabb::Union is a no-op for an
  // Aabb::Empty()-sentinel operand (min=+FLT_MAX/max=-FLT_MAX, see
  // aabb.cpp), so unioning in an empty LOD's bounds unguarded is safe --
  // matters for the known pine-preset gap where one LOD's voxelization can
  // legitimately be empty (see leaf_voxelizer.hpp) while others aren't.
  // has_leaves reflects whether ANY supplied LOD has geometry.
  Aabb leaf_bounds = Aabb::Empty();
  bool has_leaves = false;
  for (const TexturedMeshResult& m : leaf_lod_meshes) {
    if (m.mesh.vertex_count > 0) {
      has_leaves = true;
    }
    leaf_bounds = leaf_bounds.Union(m.local_bounds);
  }
  tf->leaf_local_bounds = leaf_bounds;
  tf->has_leaves = has_leaves;

  for (uint32_t lod = 0; lod < GpuInstanceRenderer::kMaxLods; ++lod) {
    StaticTexturedMeshComponent bark_mesh = bark_lod0.mesh;

    if (kDefaultLodRatios[lod] < 1.0f) {
      SimplifyInPlace(bark_mesh, kDefaultLodRatios[lod]);
    }

    if (bark_mesh.indices.empty()) {
      spdlog::error("BuildTreeField: empty bark mesh at lod {}", lod);
      return nullptr;
    }

    const uint32_t bucket = GpuInstanceRenderer::BucketId(/*model=*/0u, lod);
    TreeField::LodBuffers& buffers = tf->lod_buffers[lod];

    // --- Bark submesh (0, kDeferred + kShadow). ---
    buffers.bark_vertex_buffer = UploadVertexBuffer(device, bark_mesh.vertices);
    buffers.bark_index_buffer = UploadIndexBuffer(device, bark_mesh.indices);
    const uint32_t bark_index_count =
        static_cast<uint32_t>(bark_mesh.indices.size());

    InstanceParams bark_params;
    // instanced_gbuffer's group-0 texture bindings (albedo@1, normal@3,
    // arm@4 -- see shaders/material/instanced_gbuffer.wesl) have no
    // material_requirements.cpp entry, so StandardMaterialFactory derives
    // slot names from reflection as "tex_<binding>" (see the .hpp's
    // deviation note). Albedo (tex_1) is left unset -- its "white" factory
    // default * `tint` below reproduces a flat solid bark color. Reused
    // as-is for the shadow-pass instance below (its bindings are declared
    // unconditionally in the WESL, not gated on shadow_pass -- see the
    // .wesl file) -- bucketId is what the shadow vertex path actually needs
    // (bucketBase[mat_params.bucketId]).
    bark_params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "tex_3",
        .view = tf->bark_normal_view,
        .sampler = tf->bark_support_sampler,
        .type = TextureType::k2D,
    });
    bark_params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "tex_4",
        .view = tf->bark_arm_view,
        .sampler = tf->bark_support_sampler,
        .type = TextureType::k2D,
    });
    bark_params.uniform_overrides["tint"] =
        MaterialParameterValue(glm::vec4(0.30f, 0.19f, 0.10f, 1.0f));
    bark_params.uniform_overrides["bucketId"] = MaterialParameterValue(bucket);

    entt::id_type bark_key = ComposeMaterialCacheKey(
        entt::hashed_string{"tree_bark"}.value(), GeometryType::kInstancedMesh,
        RenderPassType::kGBuffer, bucket);
    auto bark_handle = tf->material_cache.GetOrCreate(
        bark_key, *tf->bark_factory, GeometryType::kInstancedMesh,
        MaterialPassType::kDeferred, RenderPassType::kGBuffer, bark_params);
    if (!bark_handle) {
      spdlog::error("BuildTreeField: bark material resolve failed at lod {}",
                    lod);
      return nullptr;
    }
    tf->material_handles.push_back(bark_handle);
    tf->field->SetSubmesh(/*model=*/0u, lod, /*submesh=*/0u,
                          buffers.bark_vertex_buffer,
                          buffers.bark_index_buffer, wgpu::IndexFormat::Uint32,
                          bark_index_count,
                          InstancedMeshField::PassKind::kDeferred,
                          bark_handle.operator->());

    entt::id_type bark_shadow_key = ComposeMaterialCacheKey(
        entt::hashed_string{"tree_bark_shadow"}.value(),
        GeometryType::kInstancedMesh, RenderPassType::kShadow, bucket);
    auto bark_shadow_handle = tf->material_cache.GetOrCreate(
        bark_shadow_key, *tf->bark_factory, GeometryType::kInstancedMesh,
        MaterialPassType::kDeferred, RenderPassType::kShadow, bark_params);
    if (!bark_shadow_handle) {
      spdlog::error(
          "BuildTreeField: bark shadow material resolve failed at lod {}",
          lod);
      return nullptr;
    }
    tf->material_handles.push_back(bark_shadow_handle);
    tf->field->SetSubmeshShadow(/*model=*/0u, lod, /*submesh=*/0u,
                                bark_shadow_handle.operator->());

    // --- Leaf submesh (1, kDeferred + kShadow), only for a LOD whose
    // supplied mesh actually has geometry -- a LOD can legally voxelize
    // empty (see leaf_voxelizer.hpp's known gap), in which case this slot is
    // simply left unconfigured: GpuInstanceRenderer::Draw skips zero-
    // index-count slots automatically, so this is safe (no dead draw, no
    // validation error) without any special-casing at draw time. A tree with
    // leaves at OTHER LODs hitting this at lod N still renders bare there
    // (fail-soft -- pine-at-dead-zone during dev must not brick the viewer),
    // but spdlog::warn's so the gap has a diagnostic instead of silently
    // popping the crown at that distance band. ---
    const StaticTexturedMeshComponent& leaf_mesh = leaf_lod_meshes[lod].mesh;
    if (leaf_mesh.vertex_count > 0 && !leaf_mesh.indices.empty()) {
      buffers.leaf_vertex_buffer =
          UploadVertexBuffer(device, leaf_mesh.vertices);
      buffers.leaf_index_buffer = UploadIndexBuffer(device, leaf_mesh.indices);
      const uint32_t leaf_index_count =
          static_cast<uint32_t>(leaf_mesh.indices.size());

      InstanceParams leaf_params;
      // voxel_foliage declares no textures at all (see the .wesl) -- no
      // texture_overrides needed. `params`: x = roughness, y = translucency
      // strength (shaders/material/voxel_foliage.wesl's MaterialParams).
      // Reused as-is for the shadow-pass instance below (voxel_foliage's
      // shadow_pass vertex stage still reads mat_params.bucketId).
      leaf_params.uniform_overrides["tint"] =
          MaterialParameterValue(glm::vec4(options.leaves.tint, 1.0f));
      leaf_params.uniform_overrides["params"] = MaterialParameterValue(
          glm::vec4(kBarkRoughness, options.leaves.transmission_strength,
                    0.0f, 0.0f));
      leaf_params.uniform_overrides["bucketId"] =
          MaterialParameterValue(bucket);

      entt::id_type leaf_key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_leaf"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kGBuffer, bucket);
      auto leaf_handle = tf->material_cache.GetOrCreate(
          leaf_key, *tf->leaf_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kDeferred, RenderPassType::kGBuffer, leaf_params);
      if (!leaf_handle) {
        spdlog::error(
            "BuildTreeField: leaf material resolve failed at lod {}", lod);
        return nullptr;
      }
      tf->material_handles.push_back(leaf_handle);
      tf->field->SetSubmesh(/*model=*/0u, lod, /*submesh=*/1u,
                            buffers.leaf_vertex_buffer,
                            buffers.leaf_index_buffer,
                            wgpu::IndexFormat::Uint32, leaf_index_count,
                            InstancedMeshField::PassKind::kDeferred,
                            leaf_handle.operator->());

      entt::id_type leaf_shadow_key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_leaf_shadow"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kShadow, bucket);
      auto leaf_shadow_handle = tf->material_cache.GetOrCreate(
          leaf_shadow_key, *tf->leaf_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kDeferred, RenderPassType::kShadow, leaf_params);
      if (!leaf_shadow_handle) {
        spdlog::error(
            "BuildTreeField: leaf shadow material resolve failed at lod {}",
            lod);
        return nullptr;
      }
      tf->material_handles.push_back(leaf_shadow_handle);
      tf->field->SetSubmeshShadow(/*model=*/0u, lod, /*submesh=*/1u,
                                  leaf_shadow_handle.operator->());
    } else if (has_leaves) {
      spdlog::warn(
          "BuildTreeField: lod {} supplied an empty leaf voxelization (tree "
          "has leaves at other LODs) -- rendering bare at this LOD",
          lod);
    }
  }

  return tf;
}

}  // namespace badlands
