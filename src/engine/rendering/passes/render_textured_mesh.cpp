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
#include "engine/rendering/material/material_instance_cache.hpp"

namespace badlands {

void RenderTexturedMeshes(RenderPassContext& pass, FrameContext& frame,
                          entt::registry& registry,
                          const glm::vec3& camera_world_pos,
                          RenderPassType render_pass_type,
                          MaterialInstanceCache& cache) {
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

      registry.emplace_or_replace<StaticTexturedMeshGpuComponent>(
          entity, StaticTexturedMeshGpuComponent{
                      .vertex_buffer = std::move(vertex_buffer),
                      .vertex_count = mesh.vertex_count,
                  });

      mesh.dirty = false;
    }

    auto* gpu = registry.try_get<StaticTexturedMeshGpuComponent>(entity);
    if (!gpu || !gpu->vertex_buffer || gpu->vertex_count == 0) {
      continue;
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

    // Model matrix, rebased to camera-offset space for float precision.
    glm::mat4 model_transform = mesh.transform;
    if (auto* xform = registry.try_get<Transform>(entity)) {
      model_transform = xform->matrix;
    }
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
    pass.Draw(gpu->vertex_count);
  }
}

}  // namespace badlands
