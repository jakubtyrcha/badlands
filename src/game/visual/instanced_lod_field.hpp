#pragma once

// Builds an InstancedMeshField from N InstancedLodModels: uploads every
// (model, lod, submesh) mesh, resolves each submesh's material, and wires the
// per-model LOD chain the GPU selects on.
//
// Game-side rather than engine-side because it composes MATERIALS (factories,
// a cache, shader names), which the engine layer deliberately knows nothing
// about -- InstancedMeshField takes already-resolved material instances and
// never reaches into a cache itself.
//
// This used to be BuildTreeField and was shaped around exactly two submeshes,
// bark and a voxel crown, with their factories and decimation policy hardcoded.
// Nothing here knows what a tree is any more; see instanced_lod_model.hpp for
// the producers that do.
//
// WHAT THE BUILDER OWNS, and why it has to. InstancedMeshField::SetSubmesh does
// NOT take ownership of the vertex/index buffers or the material it is handed,
// and MaterialInstanceCache handles keep instances alive only while held. So
// every GPU resource the field references lives in the returned
// InstancedLodField and dies with it.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/rendering/instanced_mesh_field.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "game/visual/impostor_atlas.hpp"
#include "game/visual/instanced_lod_model.hpp"

namespace badlands {

class GpuPipelineGenerator;

// The optional impostor level, appended on top of every model's mesh chain.
//
// This is the BAKED RESULT, not InstancedLodModel::impostor (which describes
// how to bake). The bake runs between the two, so they are deliberately
// separate types rather than one member.
struct InstancedLodImpostor {
  const ImpostorAtlas* atlas = nullptr;
  std::span<const ImpostorPlacement> placement;

  bool active(size_t model_count) const {
    return atlas != nullptr && atlas->valid() &&
           placement.size() == model_count;
  }
};

// Owns an InstancedMeshField configured for N models, each with its OWN runtime
// LOD count and cutoffs (GpuInstanceRenderer::ModelLod is per model), bounded
// by the engine's compile-time kMaxLods cap.
struct InstancedLodField {
  std::unique_ptr<InstancedMeshField> field;

  // One factory per distinct (shader_name, cull_mode, casts_shadow). Deduped
  // because a factory compiles pipelines: a 28-model forest whose models all
  // name the same two shaders must build two factories, not fifty-six.
  std::vector<std::unique_ptr<MaterialInstanceFactory>> factories;
  MaterialInstanceCache material_cache;
  // Keeps every resolved instance alive for this field's lifetime.
  std::vector<entt::resource<RenderingMaterialInstance>> material_handles;

  // Solid-colour support textures created from SolidColorTextureSpec, deduped
  // by rgba across every model and submesh.
  std::vector<wgpu::TextureView> solid_texture_views;
  wgpu::Sampler solid_texture_sampler;

  struct SubmeshBuffers {
    wgpu::Buffer vertex_buffer;
    wgpu::Buffer index_buffer;
    uint32_t index_count = 0;
  };
  // Indexed [model][lod][submesh]. The inner extents are that model's own
  // runtime counts, NOT kMaxLods -- that constant is only the cap.
  std::vector<std::vector<std::vector<SubmeshBuffers>>> buffers;

  // The impostor level's shared unit quad (identical for every model -- only
  // the material differs) and its factory. Null unless an impostor was given.
  std::unique_ptr<MaterialInstanceFactory> impostor_factory;
  wgpu::Buffer impostor_vertex_buffer;
  wgpu::Buffer impostor_index_buffer;

  // Per-model, per-submesh local bounds in NATIVE units, parallel to the
  // `models` span. See LodModelBounds for why this is not a single box.
  std::vector<LodModelBounds> model_bounds;
  // Copied from each model so a caller building instance transforms need not
  // keep the inputs alive.
  std::vector<float> native_to_world_scale;

  size_t model_count() const { return model_bounds.size(); }
};

// Builds a field from N already-prepared models.
//
// Each model contributes its own ModelLod chain (level count = levels.size(),
// cutoffs = thresholds), so models with different LOD counts coexist in one
// field. The field's submesh count is the MAX across models; a model with
// fewer simply leaves the extra slots unconfigured, which is legal --
// GpuInstanceRenderer::Draw skips zero-index-count slots.
//
// An empty (model, lod, submesh) mesh is likewise legal and leaves that slot
// unconfigured. It is warned about only when the SAME submesh has geometry at
// some other LOD, since that is the case where a level silently loses a part
// the neighbouring levels draw (the known voxel-crown gap); a submesh that is
// empty everywhere is simply a model with fewer parts.
//
// Returns nullptr, after logging which model and which rule, if `models` is
// empty or any model fails ValidateLodModel, or on any factory-build,
// field-compile, upload or material-resolve failure.
std::unique_ptr<InstancedLodField> BuildInstancedLodField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    std::span<const InstancedLodModel> models, uint32_t capacity,
    const InstancedLodImpostor& impostor = {});

}  // namespace badlands
