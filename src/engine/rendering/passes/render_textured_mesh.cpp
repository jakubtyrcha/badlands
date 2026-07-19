// Ported (reconciled, see the header's deviation note) from sampo's
// src/rendering/passes/render_textured_mesh.cpp, namespace sampo ->
// badlands.
#include "engine/rendering/passes/render_textured_mesh.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/components/transform.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/frustum.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"

namespace badlands {

void RenderTexturedMeshes(RenderPassContext& pass, FrameContext& frame,
                          entt::registry& registry,
                          const glm::vec3& camera_world_pos,
                          RenderPassType render_pass_type,
                          MaterialInstanceCache& cache,
                          const Frustum& frustum) {
  auto view =
      registry.view<StaticTexturedMeshComponent, MaterialFactoryComponent>();
  if (view.size_hint() == 0) {
    return;
  }

  wgpu::Device device = frame.GetDevice();

  for (auto entity : view) {
    auto& mesh = view.get<StaticTexturedMeshComponent>(entity);
    auto& fmc = view.get<MaterialFactoryComponent>(entity);

    if (!fmc.factory) {
      continue;
    }

    // Lazily (re)upload the vertex buffer. No GpuResourceManager here (see
    // StaticTexturedMeshGpuComponent's deviation note in
    // mesh_components.hpp): Dawn's wgpu::Buffer is ref-counted, so
    // overwriting/destroying the component's old buffer releases it safely.
    if (mesh.dirty && !mesh.vertices.empty()) {
      size_t buffer_size = mesh.vertices.size() * sizeof(float);

      wgpu::BufferDescriptor buf_desc;
      buf_desc.size = buffer_size;
      buf_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
      buf_desc.mappedAtCreation = true;
      buf_desc.label =
          WGPUStringView{.data = "TexturedMesh_VertexBuffer", .length = 25};
      wgpu::Buffer vertex_buffer = device.CreateBuffer(&buf_desc);

      if (!vertex_buffer) {
        spdlog::error(
            "RenderTexturedMeshes: failed to create vertex buffer ({} "
            "bytes)",
            buffer_size);
        mesh.dirty = false;
        continue;
      }

      std::memcpy(vertex_buffer.GetMappedRange(0, buf_desc.size),
                  mesh.vertices.data(), buf_desc.size);
      vertex_buffer.Unmap();

      // Optional index buffer (for DrawIndexed terrain chunks).
      wgpu::Buffer index_buffer;
      uint32_t index_count = 0;
      if (!mesh.indices.empty()) {
        const size_t idx_size = mesh.indices.size() * sizeof(uint32_t);
        wgpu::BufferDescriptor idx_desc;
        idx_desc.size = idx_size;
        idx_desc.usage =
            wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        idx_desc.mappedAtCreation = true;
        idx_desc.label =
            WGPUStringView{.data = "TexturedMesh_IndexBuffer", .length = 24};
        index_buffer = device.CreateBuffer(&idx_desc);
        if (index_buffer) {
          std::memcpy(index_buffer.GetMappedRange(0, idx_size),
                      mesh.indices.data(), idx_size);
          index_buffer.Unmap();
          index_count = static_cast<uint32_t>(mesh.indices.size());
        }
      }

      registry.emplace_or_replace<StaticTexturedMeshGpuComponent>(
          entity, StaticTexturedMeshGpuComponent{
                      .vertex_buffer = std::move(vertex_buffer),
                      .vertex_count = mesh.vertex_count,
                      .index_buffer = std::move(index_buffer),
                      .index_count = index_count,
                  });

      mesh.dirty = false;
    }

    auto* gpu = registry.try_get<StaticTexturedMeshGpuComponent>(entity);
    if (!gpu || !gpu->vertex_buffer || gpu->vertex_count == 0) {
      continue;
    }

    // Model matrix (used both for the frustum-cull AABB transform and the
    // camera-offset rebase below).
    glm::mat4 model_transform = mesh.transform;
    if (auto* xform = registry.try_get<Transform>(entity)) {
      model_transform = xform->matrix;
    }

    // Frustum cull before the (non-trivial) material resolve.
    if (auto* aabb = registry.try_get<StaticMeshAabbComponent>(entity)) {
      if (!frustum.Intersects(aabb->local.TransformedBy(model_transform))) {
        continue;
      }
    }

    // Resolve (or reuse) the material instance via factory + cache.
    entt::id_type cache_key = ComposeMaterialCacheKey(
        entt::hashed_string::value(
            reinterpret_cast<const char*>(&fmc.factory), sizeof(void*)),
        mesh.geometry_type, render_pass_type, fmc.config_hash);

    auto instance_handle =
        cache.GetOrCreate(cache_key, *fmc.factory, mesh.geometry_type,
                          fmc.pass_type, render_pass_type, fmc.params);

    if (!instance_handle || !instance_handle->IsValid()) {
      continue;
    }
    auto* instance = instance_handle.operator->();

    // Rebase the model matrix to camera-offset space for float precision.
    glm::vec3 world_pos(model_transform[3]);
    glm::mat4 offset_transform = model_transform;
    offset_transform[3] = glm::vec4(world_pos - camera_world_pos, 1.0f);

    instance->SetParameterByName("modelMatrix",
                                 MaterialParameterValue(offset_transform));

    // Transfer uniform overrides from the factory component.
    for (const auto& [name, value] : fmc.params.uniform_overrides) {
      instance->SetParameterByName(name, value);
    }

    // Bind pipeline + group 0 (frame uniforms + textures), then group 1
    // (per-object uniforms).
    if (!instance->Bind(pass, frame)) {
      continue;
    }
    if (!instance->BindPerObject(pass, frame)) {
      continue;
    }

    pass.SetVertexBuffer(0, gpu->vertex_buffer);
    if (gpu->index_count > 0) {
      pass.SetIndexBuffer(gpu->index_buffer, wgpu::IndexFormat::Uint32);
      pass.DrawIndexed(gpu->index_count);
    } else {
      pass.Draw(gpu->vertex_count);
    }
  }
}

}  // namespace badlands
