// Forward render passes. See render_forward.hpp. The lazy vertex upload mirrors
// render_textured_mesh.cpp (Dawn buffers are ref-counted; overwriting the GPU
// component releases the old buffer). Both variants additionally bind
// engine-owned resources at @group(2) when the material declares it (see
// ForwardEngineResources): the opaque pass binds a purpose-fit 6-entry group
// (shadow map + IBL only — no scene depth/color, since scene_depth is also
// the opaque pass's writable depth attachment and scene_color is stale at
// opaque time), while the transparent pass binds the full 9-entry group
// (adds scene depth/color, which water genuinely needs).
#include "engine/rendering/passes/render_forward.hpp"

#include <array>
#include <cstring>

#include <spdlog/spdlog.h>

#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/components/transform.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"

namespace badlands {

namespace {

// Lazily (re)upload a dirty mesh's vertex buffer. Returns false if there's no
// drawable buffer. Mirrors render_textured_mesh.cpp.
bool UploadIfNeeded(entt::registry& registry, entt::entity entity,
                    StaticTexturedMeshComponent& mesh, wgpu::Device device) {
  if (mesh.dirty && !mesh.vertices.empty()) {
    size_t buffer_size = mesh.vertices.size() * sizeof(float);
    wgpu::BufferDescriptor buf_desc;
    buf_desc.size = buffer_size;
    buf_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    buf_desc.mappedAtCreation = true;
    buf_desc.label =
        WGPUStringView{.data = "Forward_VertexBuffer", .length = 20};
    wgpu::Buffer vertex_buffer = device.CreateBuffer(&buf_desc);
    if (!vertex_buffer) {
      mesh.dirty = false;
      return false;
    }
    std::memcpy(vertex_buffer.GetMappedRange(0, buf_desc.size),
                mesh.vertices.data(), buf_desc.size);
    vertex_buffer.Unmap();

    // Optional index buffer (mirrors render_textured_mesh.cpp). Meshes with
    // no indices leave index_count 0 (unindexed Draw).
    wgpu::Buffer index_buffer;
    uint32_t index_count = 0;
    if (!mesh.indices.empty()) {
      const size_t idx_size = mesh.indices.size() * sizeof(uint32_t);
      wgpu::BufferDescriptor idx_desc;
      idx_desc.size = idx_size;
      idx_desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
      idx_desc.mappedAtCreation = true;
      idx_desc.label =
          WGPUStringView{.data = "Forward_IndexBuffer", .length = 19};
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
  return gpu && gpu->vertex_buffer && gpu->vertex_count > 0;
}

// Resolve the cached material instance and set its per-object params. Returns
// nullptr if the instance can't be created.
RenderingMaterialInstance* ResolveInstance(entt::registry& registry,
                                           entt::entity entity,
                                           StaticTexturedMeshComponent& mesh,
                                           MaterialFactoryComponent& fmc,
                                           const glm::vec3& camera_world_pos,
                                           MaterialInstanceCache& cache) {
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string::value(reinterpret_cast<const char*>(&fmc.factory),
                                 sizeof(void*)),
      mesh.geometry_type, RenderPassType::kForward, fmc.config_hash);
  auto handle = cache.GetOrCreate(key, *fmc.factory, mesh.geometry_type,
                                  fmc.pass_type, RenderPassType::kForward,
                                  fmc.params);
  if (!handle || !handle->IsValid()) {
    return nullptr;
  }
  auto* instance = handle.operator->();

  glm::mat4 model = mesh.transform;
  if (auto* xform = registry.try_get<Transform>(entity)) {
    model = xform->matrix;
  }
  glm::vec3 world_pos(model[3]);
  model[3] = glm::vec4(world_pos - camera_world_pos, 1.0f);
  instance->SetParameterByName("modelMatrix", MaterialParameterValue(model));
  for (const auto& [name, value] : fmc.params.uniform_overrides) {
    instance->SetParameterByName(name, value);
  }
  return instance;
}

// Build the @group(2) engine bind group (identical layout/entries for every
// draw that declares it). Shared by RenderForwardMeshes and
// RenderForwardTransparentMeshes.
wgpu::BindGroup BuildForwardEngineBindGroup(RenderingMaterialInstance* instance,
                                            FrameContext& frame,
                                            const ForwardEngineResources& engine) {
  std::array<wgpu::BindGroupEntry, 9> entries{};
  entries[0].binding = 0;
  entries[0].textureView = engine.scene_depth;
  entries[1].binding = 1;
  entries[1].textureView = engine.scene_color;
  entries[2].binding = 2;
  entries[2].sampler = engine.scene_color_sampler;
  entries[3].binding = 3;
  entries[3].textureView = engine.ibl_prefiltered;
  entries[4].binding = 4;
  entries[4].sampler = engine.ibl_sampler;
  entries[5].binding = 5;
  entries[5].textureView = engine.brdf_lut;
  entries[6].binding = 6;
  entries[6].sampler = engine.brdf_lut_sampler;
  entries[7].binding = 7;
  entries[7].textureView = engine.shadow_map;
  entries[8].binding = 8;
  entries[8].sampler = engine.shadow_sampler;
  return frame.CreateBindGroup(instance->GetPipeline().GetBindGroupLayout(2),
                                entries);
}

// Build the @group(2) engine bind group for the forward-OPAQUE pass: shadow
// map + IBL only (no scene_depth/scene_color — scene_depth is also the
// opaque pass's writable depth attachment, a Dawn read-write aliasing hazard,
// and scene_color is stale at opaque time). Used by RenderForwardMeshes only;
// RenderForwardTransparentMeshes keeps the full 9-entry
// BuildForwardEngineBindGroup above (water genuinely needs scene depth/color).
wgpu::BindGroup BuildForwardOpaqueEngineBindGroup(RenderingMaterialInstance* instance,
                                                  FrameContext& frame,
                                                  const ForwardEngineResources& engine) {
  std::array<wgpu::BindGroupEntry, 6> entries{};
  entries[0].binding = 0; entries[0].textureView = engine.shadow_map;
  entries[1].binding = 1; entries[1].sampler     = engine.shadow_sampler;
  entries[2].binding = 2; entries[2].textureView = engine.ibl_prefiltered;
  entries[3].binding = 3; entries[3].sampler     = engine.ibl_sampler;
  entries[4].binding = 4; entries[4].textureView = engine.brdf_lut;
  entries[5].binding = 5; entries[5].sampler     = engine.brdf_lut_sampler;
  return frame.CreateBindGroup(instance->GetPipeline().GetBindGroupLayout(2), entries);
}

}  // namespace

void RenderForwardMeshes(RenderPassContext& pass, FrameContext& frame,
                         entt::registry& registry,
                         const glm::vec3& camera_world_pos,
                         MaterialInstanceCache& cache,
                         const ForwardEngineResources& engine) {
  wgpu::Device device = frame.GetDevice();
  const bool have_engine = static_cast<bool>(engine.scene_depth);

  // The @group(2) engine bind group is identical for every draw, so build it
  // once (lazily, from the first material that declares group 2). Mirrors
  // RenderForwardTransparentMeshes below.
  wgpu::BindGroup engine_bg;

  auto view = registry.view<StaticTexturedMeshComponent, MaterialFactoryComponent,
                            ForwardOpaqueRenderable>();
  for (auto entity : view) {
    auto& mesh = view.get<StaticTexturedMeshComponent>(entity);
    auto& fmc = view.get<MaterialFactoryComponent>(entity);
    if (!fmc.factory || !UploadIfNeeded(registry, entity, mesh, device)) {
      continue;
    }
    auto* instance =
        ResolveInstance(registry, entity, mesh, fmc, camera_world_pos, cache);
    if (!instance || !instance->Bind(pass, frame) ||
        !instance->BindPerObject(pass, frame)) {
      continue;
    }

    // A forward-opaque material declares @group(2) iff it needs the engine
    // resources. Others skip the bind — binding a non-existent group would
    // fail Dawn validation.
    const bool wants_engine = have_engine && instance->DeclaresBindGroup(2);
    if (wants_engine) {
      if (!engine_bg) {
        engine_bg = BuildForwardOpaqueEngineBindGroup(instance, frame, engine);
      }
      pass.SetBindGroup(2, engine_bg);
    }

    auto& gpu = registry.get<StaticTexturedMeshGpuComponent>(entity);
    pass.SetVertexBuffer(0, gpu.vertex_buffer);
    if (gpu.index_count > 0) {
      pass.SetIndexBuffer(gpu.index_buffer, wgpu::IndexFormat::Uint32);
      pass.DrawIndexed(gpu.index_count);
    } else {
      pass.Draw(gpu.vertex_count);
    }
  }
}

void RenderForwardTransparentMeshes(RenderPassContext& pass, FrameContext& frame,
                                    entt::registry& registry,
                                    const glm::vec3& camera_world_pos,
                                    MaterialInstanceCache& cache,
                                    const ForwardEngineResources& engine) {
  wgpu::Device device = frame.GetDevice();
  const bool have_engine = static_cast<bool>(engine.scene_depth);

  // The @group(2) engine bind group is identical for every draw, so build it
  // once (lazily, from the first material that declares group 2). Assumes any
  // group-2 transparent material in a frame shares the layout (only water today).
  wgpu::BindGroup engine_bg;

  auto view = registry.view<StaticTexturedMeshComponent, MaterialFactoryComponent,
                            ForwardTransparentRenderable>();
  for (auto entity : view) {
    auto& mesh = view.get<StaticTexturedMeshComponent>(entity);
    auto& fmc = view.get<MaterialFactoryComponent>(entity);
    if (!fmc.factory || !UploadIfNeeded(registry, entity, mesh, device)) {
      continue;
    }
    auto* instance =
        ResolveInstance(registry, entity, mesh, fmc, camera_world_pos, cache);
    if (!instance) {
      continue;
    }
    // A transparent material declares @group(2) iff it needs the engine
    // resources (water). Others (e.g. plain coloured glass) skip the bind —
    // binding a non-existent group would fail Dawn validation.
    const bool wants_engine = have_engine && instance->DeclaresBindGroup(2);
    if (wants_engine) {
      // Engine-driven wave time overrides any stale param value.
      instance->SetParameterByName("time",
                                   MaterialParameterValue(engine.time_seconds));
    }
    if (!instance->Bind(pass, frame) || !instance->BindPerObject(pass, frame)) {
      continue;
    }

    if (wants_engine) {
      if (!engine_bg) {
        engine_bg = BuildForwardEngineBindGroup(instance, frame, engine);
      }
      pass.SetBindGroup(2, engine_bg);
    }

    auto& gpu = registry.get<StaticTexturedMeshGpuComponent>(entity);
    pass.SetVertexBuffer(0, gpu.vertex_buffer);
    if (gpu.index_count > 0) {
      pass.SetIndexBuffer(gpu.index_buffer, wgpu::IndexFormat::Uint32);
      pass.DrawIndexed(gpu.index_count);
    } else {
      pass.Draw(gpu.vertex_count);
    }
  }
}

}  // namespace badlands
