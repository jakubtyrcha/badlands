#include "game/visual/tree_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // kTexturedMeshFloatsPerVertex
#include "engine/rendering/scene_renderer.hpp"                   // kAccumulationFormat/kDepthFormat
#include "engine/rendering/texture_loader.hpp"                   // CreateSolidColorTexture
#include "game/geometry/mesh_lod.hpp"
#include "game/geometry/tree_generator.hpp"

namespace badlands {

namespace {

// mesh_lod.hpp's kDefaultLodRatios/kLeafLodRatios sized to
// GpuInstanceRenderer::kMaxLods: one ratio per LOD bucket this field builds
// below.
static_assert(kDefaultLodRatios.size() == GpuInstanceRenderer::kMaxLods);
static_assert(kLeafLodRatios.size() == GpuInstanceRenderer::kMaxLods);

// Matte roughness for the bark ARM texture -- same rationale/value as
// model_viewer_view.cpp's single-tree bark_mat_ (SolidColor(..., 0.9f)).
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
    const TreeOptions& options, wgpu::TextureView leaf_view,
    wgpu::Sampler leaf_sampler, uint32_t capacity,
    std::array<float, GpuInstanceRenderer::kMaxLods - 1> lod_thresholds) {
  auto tf = std::make_unique<TreeField>();

  // --- Bark factory: instanced_gbuffer / kDeferred, no shadow casting yet
  // (see instanced_mesh_field.hpp's "out of scope" note). ---
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
    desc.casts_shadow = false;
    tf->bark_factory = BuildMaterialInstanceFactory(desc, device, queue,
                                                     &pipeline_gen);
    if (!tf->bark_factory) {
      spdlog::error(
          "BuildTreeField: failed to build instanced_gbuffer factory");
      return nullptr;
    }
  }

  // --- Leaf factory: instanced_forward / kForwardOpaque, mirrors
  // MaterialLibrary::TranslucentFoliage's descriptor (material_library.cpp)
  // but on kInstancedMesh geometry + no shadow casting. ---
  {
    FactoryDescriptor desc;
    desc.shader_name = "instanced_forward";
    desc.shader_path = "material/instanced_forward";
    desc.supported_pass_types = {MaterialPassType::kForwardOpaque};
    desc.supported_geometry_types = {GeometryType::kInstancedMesh};
    desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
    desc.depth_format = SceneRenderer::kDepthFormat;
    desc.cull_mode = wgpu::CullMode::None;  // double-sided
    desc.casts_shadow = false;
    desc.extra_features = {"translucency"};
    tf->leaf_factory = BuildMaterialInstanceFactory(desc, device, queue,
                                                     &pipeline_gen);
    if (!tf->leaf_factory) {
      spdlog::error(
          "BuildTreeField: failed to build instanced_forward factory");
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

  // Generate the bark/leaf mesh ONCE at LOD0 -- generation is deterministic,
  // so a fresh GenerateTreeMesh/GenerateLeafMesh call per lod (as this used
  // to do) would reproduce byte-identical vertices/indices at 3x the cost.
  // LOD1/2 below simplify a COPY of this LOD0 data at the shared ratio
  // (kDefaultLodRatios), the single-tree path's exact pattern
  // (model_viewer_view.cpp).
  TexturedMeshResult bark_lod0 = GenerateTreeMesh(options, skeleton);
  TexturedMeshResult leaves_lod0 = GenerateLeafMesh(options, skeleton);
  const bool has_leaves = leaves_lod0.mesh.vertex_count > 0;
  tf->bark_local_bounds = bark_lod0.local_bounds;
  tf->leaf_local_bounds = leaves_lod0.local_bounds;
  tf->has_leaves = has_leaves;

  for (uint32_t lod = 0; lod < GpuInstanceRenderer::kMaxLods; ++lod) {
    StaticTexturedMeshComponent bark_mesh = bark_lod0.mesh;
    StaticTexturedMeshComponent leaves_mesh = leaves_lod0.mesh;

    if (kDefaultLodRatios[lod] < 1.0f) {
      SimplifyInPlace(bark_mesh, kDefaultLodRatios[lod]);
    }
    if (has_leaves && kLeafLodRatios[lod] < 1.0f) {
      SimplifyInPlace(leaves_mesh, kLeafLodRatios[lod]);
    }

    if (bark_mesh.indices.empty()) {
      spdlog::error("BuildTreeField: empty bark mesh at lod {}", lod);
      return nullptr;
    }

    const uint32_t bucket = GpuInstanceRenderer::BucketId(/*model=*/0u, lod);
    TreeField::LodBuffers& buffers = tf->lod_buffers[lod];

    // --- Bark submesh (0, kDeferred). ---
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
    // default * `tint` below reproduces a flat solid bark color.
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

    // --- Leaf submesh (1, kForwardOpaque), only if the tree has leaves. ---
    if (has_leaves) {
      buffers.leaf_vertex_buffer =
          UploadVertexBuffer(device, leaves_mesh.vertices);
      buffers.leaf_index_buffer = UploadIndexBuffer(device, leaves_mesh.indices);
      const uint32_t leaf_index_count =
          static_cast<uint32_t>(leaves_mesh.indices.size());

      InstanceParams leaf_params;
      // instanced_forward's single group-0 texture (albedo@1 -- see
      // shaders/material/instanced_forward.wesl) is likewise unregistered
      // in material_requirements.cpp, so its reflection-derived slot name is
      // "tex_1" (see the .hpp's deviation note), NOT "albedo".
      leaf_params.texture_overrides.push_back(DefaultTextureView{
          .param_name = "tex_1",
          .view = leaf_view,
          .sampler = leaf_sampler,
          .type = TextureType::k2D,
      });
      leaf_params.uniform_overrides["tint"] =
          MaterialParameterValue(glm::vec4(options.leaves.tint, 1.0f));
      leaf_params.uniform_overrides["params"] = MaterialParameterValue(
          glm::vec4(options.leaves.alpha_cutoff, kBarkRoughness, 0.0f, 0.0f));
      leaf_params.uniform_overrides["transmission"] =
          MaterialParameterValue(glm::vec4(options.leaves.transmission_tint,
                                           options.leaves.transmission_strength));
      leaf_params.uniform_overrides["bucketId"] =
          MaterialParameterValue(bucket);

      entt::id_type leaf_key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_leaf"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kForward, bucket);
      auto leaf_handle = tf->material_cache.GetOrCreate(
          leaf_key, *tf->leaf_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kForwardOpaque, RenderPassType::kForward,
          leaf_params);
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
                            InstancedMeshField::PassKind::kForwardOpaque,
                            leaf_handle.operator->());
    }
  }

  return tf;
}

}  // namespace badlands
