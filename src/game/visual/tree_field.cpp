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

// Mirrors the viewer's manual-LOD ratios (model_viewer_view.cpp's
// kLodRatios): LOD0 is identity (SimplifyMesh short-circuits ratio >= 1.0).
constexpr float kLodRatios[GpuInstanceRenderer::kMaxLods] = {1.0f, 0.5f, 0.2f};

// Matte roughness for the bark ARM texture -- same rationale/value as
// model_viewer_view.cpp's single-tree bark_mat_ (SolidColor(..., 0.9f)).
constexpr float kBarkRoughness = 0.9f;

wgpu::Buffer UploadBuffer(wgpu::Device device, wgpu::Queue queue,
                          const void* data, uint64_t size,
                          wgpu::BufferUsage usage) {
  if (size == 0) return nullptr;
  wgpu::BufferDescriptor desc{};
  desc.size = size;
  desc.usage = usage | wgpu::BufferUsage::CopyDst;
  desc.mappedAtCreation = true;
  wgpu::Buffer buffer = device.CreateBuffer(&desc);
  if (!buffer) return nullptr;
  std::memcpy(buffer.GetMappedRange(0, size), data, size);
  buffer.Unmap();
  return buffer;
}

wgpu::Buffer UploadVertexBuffer(wgpu::Device device, wgpu::Queue queue,
                                const std::vector<float>& vertices) {
  return UploadBuffer(device, queue, vertices.data(),
                      vertices.size() * sizeof(float),
                      wgpu::BufferUsage::Vertex);
}

wgpu::Buffer UploadIndexBuffer(wgpu::Device device, wgpu::Queue queue,
                               const std::vector<uint32_t>& indices) {
  return UploadBuffer(device, queue, indices.data(),
                      indices.size() * sizeof(uint32_t),
                      wgpu::BufferUsage::Index);
}

// Simplifies `mesh` in place to kLodRatios[lod] (a no-op call for lod==0,
// since SimplifyMesh short-circuits ratio >= 1.0 -- kept unconditional so the
// LOD0 mesh is guaranteed byte-identical to a freshly generated one).
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

  for (uint32_t lod = 0; lod < GpuInstanceRenderer::kMaxLods; ++lod) {
    TexturedMeshResult bark = GenerateTreeMesh(options, skeleton);
    TexturedMeshResult leaves = GenerateLeafMesh(options, skeleton);
    const bool has_leaves = leaves.mesh.vertex_count > 0;

    if (lod == 0) {
      tf->bark_local_bounds = bark.local_bounds;
      tf->leaf_local_bounds = leaves.local_bounds;
      tf->has_leaves = has_leaves;
    }

    if (kLodRatios[lod] < 1.0f) {
      SimplifyInPlace(bark.mesh, kLodRatios[lod]);
      if (has_leaves) {
        SimplifyInPlace(leaves.mesh, kLodRatios[lod]);
      }
    }

    if (bark.mesh.indices.empty()) {
      spdlog::error("BuildTreeField: empty bark mesh at lod {}", lod);
      return nullptr;
    }

    const uint32_t bucket = GpuInstanceRenderer::BucketId(/*model=*/0u, lod);
    TreeField::LodBuffers& buffers = tf->lod_buffers[lod];

    // --- Bark submesh (0, kDeferred). ---
    buffers.bark_vertex_buffer =
        UploadVertexBuffer(device, queue, bark.mesh.vertices);
    buffers.bark_index_buffer =
        UploadIndexBuffer(device, queue, bark.mesh.indices);
    const uint32_t bark_index_count =
        static_cast<uint32_t>(bark.mesh.indices.size());

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
          UploadVertexBuffer(device, queue, leaves.mesh.vertices);
      buffers.leaf_index_buffer =
          UploadIndexBuffer(device, queue, leaves.mesh.indices);
      const uint32_t leaf_index_count =
          static_cast<uint32_t>(leaves.mesh.indices.size());

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
