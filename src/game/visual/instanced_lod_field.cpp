#include "game/visual/instanced_lod_field.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/texture_loader.hpp"  // CreateSolidColorTexture
#include "game/visual/foliage_voxel_config.hpp"  // kFoliageImpostorThresholdPreviewM

namespace badlands {

namespace {

// Write-once buffer: contents are fully supplied via mappedAtCreation, so no
// CopyDst (it is never subsequently queue.WriteBuffer'd) and no wgpu::Queue
// parameter (the memcpy-into-mapped-range upload never touches the queue).
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
  return UploadBuffer(device, indices.data(), indices.size() * sizeof(uint32_t),
                      wgpu::BufferUsage::Index);
}

// Builds each distinct material factory exactly once.
//
// The identity is (shader_path, cull_mode, casts_shadow) -- everything a
// FactoryDescriptor varies here. Parameters are NOT part of it: those live in
// InstanceParams and are resolved per bucket through the material cache, which
// is the whole reason one factory can serve every model.
class FactoryPool {
 public:
  FactoryPool(wgpu::Device device, wgpu::Queue queue,
              GpuPipelineGenerator& pipeline_gen, InstancedLodField& out)
      : device_(device), queue_(queue), pipeline_gen_(pipeline_gen),
        out_(out) {}

  // Null (after logging) if the factory could not be built.
  MaterialInstanceFactory* Get(const InstancedMaterialSpec& spec) {
    const Key key{spec.shader_path, spec.cull_mode, spec.casts_shadow};
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] == key) return out_.factories[i].get();
    }

    FactoryDescriptor desc;
    desc.shader_name = spec.shader_name;
    desc.shader_path = spec.shader_path;
    desc.supported_pass_types = {MaterialPassType::kDeferred};
    desc.supported_geometry_types = {GeometryType::kInstancedMesh};
    desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                          GBuffer::kMaterialFormat};
    desc.depth_format = GBuffer::kDepthFormat;
    desc.cull_mode = spec.cull_mode;
    desc.casts_shadow = spec.casts_shadow;

    std::unique_ptr<MaterialInstanceFactory> factory =
        BuildMaterialInstanceFactory(desc, device_, queue_, &pipeline_gen_);
    if (!factory) {
      spdlog::error("BuildInstancedLodField: failed to build '{}' factory",
                    spec.shader_path);
      return nullptr;
    }
    MaterialInstanceFactory* raw = factory.get();
    keys_.push_back(key);
    out_.factories.push_back(std::move(factory));
    return raw;
  }

 private:
  using Key = std::tuple<std::string, wgpu::CullMode, bool>;

  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator& pipeline_gen_;
  InstancedLodField& out_;
  std::vector<Key> keys_;
};

// Creates each distinct 1x1 solid colour exactly once. A forest whose 28 models
// all ask for the same matte ARM gets one texture.
class SolidTexturePool {
 public:
  SolidTexturePool(wgpu::Device device, wgpu::Queue queue,
                   InstancedLodField& out)
      : device_(device), queue_(queue), out_(out) {
    wgpu::SamplerDescriptor desc{};
    out_.solid_texture_sampler = device.CreateSampler(&desc);
  }

  wgpu::TextureView Get(const std::array<uint8_t, 4>& rgba) {
    for (size_t i = 0; i < colors_.size(); ++i) {
      if (colors_[i] == rgba) return out_.solid_texture_views[i];
    }
    wgpu::TextureView view = CreateSolidColorTexture(device_, queue_, rgba[0],
                                                     rgba[1], rgba[2], rgba[3]);
    colors_.push_back(rgba);
    out_.solid_texture_views.push_back(view);
    return view;
  }

  wgpu::Sampler sampler() const { return out_.solid_texture_sampler; }

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  InstancedLodField& out_;
  std::vector<std::array<uint8_t, 4>> colors_;
};

// The material-cache key namespace for a submesh. Falls back to the shader
// name so a spec that leaves it unset still gets a stable, non-colliding key.
uint32_t NamespaceHash(const InstancedMaterialSpec& spec) {
  const std::string& name =
      spec.cache_namespace.empty() ? spec.shader_name : spec.cache_namespace;
  return entt::hashed_string::value(name.c_str());
}

// A submesh's InstanceParams for one bucket: the spec's uniforms and textures,
// plus the bucketId only the builder knows.
InstanceParams BuildParams(const InstancedMaterialSpec& spec, uint32_t bucket,
                           SolidTexturePool& solids) {
  InstanceParams params;
  params.texture_overrides = spec.textures;
  for (const SolidColorTextureSpec& solid : spec.solid_textures) {
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = solid.slot_name,
        .view = solids.Get(solid.rgba),
        .sampler = solids.sampler(),
        .type = TextureType::k2D,
    });
  }
  for (const auto& [name, value] : spec.uniforms) {
    params.uniform_overrides[name] = value;
  }
  // Every instanced shader's vertex stage reads
  // instances[bucketBase[mat_params.bucketId] + instance_index], so this is
  // required on the shadow variant too -- it is what the shadow vertex path
  // uses to find its transforms.
  params.uniform_overrides["bucketId"] = MaterialParameterValue(bucket);
  return params;
}

// True if `submesh` has geometry at any LOD other than `lod`. Used only to
// decide whether an empty slot deserves a warning: a submesh that is empty
// everywhere is a model with fewer parts, while one empty at a single level is
// a level that silently loses something its neighbours draw.
bool SubmeshHasGeometryElsewhere(const InstancedLodModel& model, size_t lod,
                                 size_t submesh) {
  for (size_t l = 0; l < model.levels.size(); ++l) {
    if (l == lod) continue;
    if (submesh < model.levels[l].size() &&
        model.levels[l][submesh].mesh.vertex_count > 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::unique_ptr<InstancedLodField> BuildInstancedLodField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    std::span<const InstancedLodModel> models, uint32_t capacity,
    const InstancedLodImpostor& impostor) {
  if (models.empty()) {
    spdlog::error("BuildInstancedLodField: no models supplied");
    return nullptr;
  }

  // The impostor adds exactly one level on top of every model's mesh chain.
  // Off unless a valid atlas AND a placement per model were supplied -- a
  // half-supplied impostor rendering nothing at range would be worse than none.
  const bool with_impostor = impostor.active(models.size());
  const uint32_t extra_lods = with_impostor ? 1u : 0u;

  size_t max_submeshes = 0;
  for (size_t mi = 0; mi < models.size(); ++mi) {
    const std::string problem = ValidateLodModel(models[mi]);
    if (!problem.empty()) {
      spdlog::error("BuildInstancedLodField: model {} {}", mi, problem);
      return nullptr;
    }
    // Validated per model above, but the impostor's extra level is the
    // builder's own addition and so is checked here rather than there.
    if (models[mi].lod_count() + extra_lods > GpuInstanceRenderer::kMaxLods) {
      spdlog::error(
          "BuildInstancedLodField: model {} has {} levels plus an impostor, "
          "past the engine's kMaxLods cap of {}",
          mi, models[mi].lod_count(), GpuInstanceRenderer::kMaxLods);
      return nullptr;
    }
    max_submeshes = std::max(max_submeshes, models[mi].submesh_count());
  }

  auto out = std::make_unique<InstancedLodField>();
  FactoryPool factories(device, queue, pipeline_gen, *out);
  SolidTexturePool solids(device, queue, *out);

  // --- The impostor factory + its shared unit quad. ---
  //
  // Hardcoded rather than driven by an InstancedMaterialSpec: the atlas has one
  // format and one shader that reads it, so there is nothing here for a
  // producer to vary. (The shader's name still says "foliage"; it is not
  // foliage-specific and renaming it is a separate change.)
  if (with_impostor) {
    FactoryDescriptor desc;
    desc.shader_name = "foliage_impostor";
    desc.shader_path = "game/foliage_impostor";
    desc.supported_pass_types = {MaterialPassType::kDeferred};
    desc.supported_geometry_types = {GeometryType::kInstancedMesh};
    desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                          GBuffer::kMaterialFormat};
    desc.depth_format = GBuffer::kDepthFormat;
    // A billboard has no back: it is built facing the viewer, so culling would
    // only ever discard it on a sign convention.
    desc.cull_mode = wgpu::CullMode::None;
    desc.casts_shadow = true;
    out->impostor_factory =
        BuildMaterialInstanceFactory(desc, device, queue, &pipeline_gen);
    if (!out->impostor_factory) {
      spdlog::error(
          "BuildInstancedLodField: failed to build foliage_impostor factory");
      return nullptr;
    }

    // Position is unused -- the vertex stage builds the quad from the instance
    // transform -- so only uv carries information, and it IS the local uv into
    // the baked tile. 12 floats per vertex; see kTexturedMeshFloatsPerVertex.
    const std::vector<float> quad = {
        // pos            uv          normal        tangent
        0, 0, 0,   0, 0,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   1, 0,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   0, 1,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   1, 1,   0, 0, 1,   1, 0, 0, 1,
    };
    const std::vector<uint32_t> quad_indices = {0, 1, 2, 2, 1, 3};
    out->impostor_vertex_buffer = UploadVertexBuffer(device, quad);
    out->impostor_index_buffer = UploadIndexBuffer(device, quad_indices);
    if (!out->impostor_vertex_buffer || !out->impostor_index_buffer) {
      spdlog::error("BuildInstancedLodField: impostor quad upload failed");
      return nullptr;
    }
  }

  // One LOD chain per model: its runtime level count and its own cutoffs,
  // padded out to the engine's fixed-width threshold array (entries past
  // lod_count-1 are never read -- see ModelLod).
  std::vector<GpuInstanceRenderer::ModelLod> model_lods;
  model_lods.reserve(models.size());
  for (const InstancedLodModel& model : models) {
    GpuInstanceRenderer::ModelLod chain;
    chain.lod_count = static_cast<uint32_t>(model.lod_count()) + extra_lods;
    std::vector<float> cutoffs = model.thresholds;
    if (with_impostor) {
      // Height-scaled like every other level: a taller model is legitimately
      // legible from further away, so its switch distances are further too.
      cutoffs.push_back(kFoliageImpostorThresholdPreviewM *
                        model.target_height_m / kFoliagePreviewHeight);
    }
    std::copy(cutoffs.begin(), cutoffs.end(), chain.thresholds.begin());
    model_lods.push_back(chain);
  }

  out->field = std::make_unique<InstancedMeshField>(
      device, queue, pipeline_gen, capacity,
      static_cast<uint32_t>(models.size()),
      static_cast<uint32_t>(max_submeshes), model_lods);
  if (!out->field->IsValid()) {
    spdlog::error("BuildInstancedLodField: InstancedMeshField compile failed");
    return nullptr;
  }

  out->model_bounds.resize(models.size());
  out->native_to_world_scale.resize(models.size());
  out->buffers.resize(models.size());

  for (uint32_t mi = 0; mi < static_cast<uint32_t>(models.size()); ++mi) {
    const InstancedLodModel& model = models[mi];
    out->native_to_world_scale[mi] = model.native_to_world_scale;
    out->model_bounds[mi] = ComputeLodModelBounds(model);
    out->buffers[mi].resize(model.lod_count());

    for (uint32_t lod = 0; lod < static_cast<uint32_t>(model.lod_count());
         ++lod) {
      const std::vector<TexturedMeshResult>& level = model.levels[lod];
      out->buffers[mi][lod].resize(level.size());
      const uint32_t bucket = GpuInstanceRenderer::BucketId(mi, lod);

      for (uint32_t s = 0; s < static_cast<uint32_t>(level.size()); ++s) {
        const StaticTexturedMeshComponent& mesh = level[s].mesh;
        if (mesh.vertex_count == 0 || mesh.indices.empty()) {
          // Legal: leaving the slot unconfigured makes Draw skip it, with no
          // dead draw and no validation error. Warned only when this submesh
          // has geometry at another LOD -- see SubmeshHasGeometryElsewhere.
          if (SubmeshHasGeometryElsewhere(model, lod, s)) {
            spdlog::warn(
                "BuildInstancedLodField: model {} lod {} submesh {} is empty "
                "while the same submesh has geometry at another lod -- "
                "rendering without it at this level",
                mi, lod, s);
          }
          continue;
        }

        InstancedLodField::SubmeshBuffers& bufs = out->buffers[mi][lod][s];
        bufs.vertex_buffer = UploadVertexBuffer(device, mesh.vertices);
        bufs.index_buffer = UploadIndexBuffer(device, mesh.indices);
        bufs.index_count = static_cast<uint32_t>(mesh.indices.size());
        if (!bufs.vertex_buffer || !bufs.index_buffer) {
          spdlog::error(
              "BuildInstancedLodField: mesh upload failed at model {} lod {} "
              "submesh {}",
              mi, lod, s);
          return nullptr;
        }

        const InstancedMaterialSpec& spec = model.submesh_materials[s];
        MaterialInstanceFactory* factory = factories.Get(spec);
        if (!factory) return nullptr;

        const InstanceParams params = BuildParams(spec, bucket, solids);
        const uint32_t ns = NamespaceHash(spec);

        // The cache key already encodes `bucket`, i.e. (model, lod), so every
        // model gets its own instances without a second key dimension.
        auto handle = out->material_cache.GetOrCreate(
            ComposeMaterialCacheKey(ns, GeometryType::kInstancedMesh,
                                    RenderPassType::kGBuffer, bucket),
            *factory, GeometryType::kInstancedMesh, MaterialPassType::kDeferred,
            RenderPassType::kGBuffer, params);
        if (!handle) {
          spdlog::error(
              "BuildInstancedLodField: material resolve failed at model {} lod "
              "{} submesh {} ('{}')",
              mi, lod, s, spec.shader_path);
          return nullptr;
        }
        out->material_handles.push_back(handle);
        out->field->SetSubmesh(mi, lod, s, bufs.vertex_buffer, bufs.index_buffer,
                               wgpu::IndexFormat::Uint32, bufs.index_count,
                               InstancedMeshField::PassKind::kDeferred,
                               handle.operator->());

        // MUST follow SetSubmesh: that call resets the slot's whole SlotInfo,
        // including any shadow material -- see instanced_mesh_field.hpp's
        // ORDER CONTRACT.
        if (!spec.casts_shadow) continue;
        auto shadow_handle = out->material_cache.GetOrCreate(
            ComposeMaterialCacheKey(ns, GeometryType::kInstancedMesh,
                                    RenderPassType::kShadow, bucket),
            *factory, GeometryType::kInstancedMesh, MaterialPassType::kDeferred,
            RenderPassType::kShadow, params);
        if (!shadow_handle) {
          spdlog::error(
              "BuildInstancedLodField: shadow material resolve failed at model "
              "{} lod {} submesh {} ('{}')",
              mi, lod, s, spec.shader_path);
          return nullptr;
        }
        out->material_handles.push_back(shadow_handle);
        out->field->SetSubmeshShadow(mi, lod, s, shadow_handle.operator->());
      }
    }

    // --- The impostor level. ---
    //
    // Submesh 0 only, and it carries the WHOLE model: the atlas baked every
    // submesh together, so there is nothing to split. Leaving the other submesh
    // slots unconfigured is legal (Draw skips zero-index-count slots).
    if (!with_impostor) continue;

    const uint32_t lod = static_cast<uint32_t>(model.lod_count());
    const uint32_t bucket = GpuInstanceRenderer::BucketId(mi, lod);
    const ImpostorPlacement& place = impostor.placement[mi];

    InstanceParams params;
    if (!BindImpostorAtlas(*impostor.atlas, params)) return nullptr;
    params.uniform_overrides["placement"] =
        MaterialParameterValue(glm::vec4(place.local_center, place.radius));
    // params.w is the atlas LAYER, and the atlas is one layer per model, so it
    // is the model id. Carried as a float so the instanced and non-instanced
    // variants share one field list (see the shader).
    params.uniform_overrides["params"] = MaterialParameterValue(
        glm::vec4(model.impostor.roughness, model.impostor.transmission_strength,
                  kImpostorAlphaCutoff, static_cast<float>(mi)));
    params.uniform_overrides["bucketId"] = MaterialParameterValue(bucket);

    auto handle = out->material_cache.GetOrCreate(
        ComposeMaterialCacheKey(entt::hashed_string{"lod_impostor"}.value(),
                                GeometryType::kInstancedMesh,
                                RenderPassType::kGBuffer, bucket),
        *out->impostor_factory, GeometryType::kInstancedMesh,
        MaterialPassType::kDeferred, RenderPassType::kGBuffer, params);
    if (!handle) {
      spdlog::error(
          "BuildInstancedLodField: impostor material resolve failed at model {}",
          mi);
      return nullptr;
    }
    out->material_handles.push_back(handle);
    out->field->SetSubmesh(mi, lod, /*submesh=*/0u, out->impostor_vertex_buffer,
                           out->impostor_index_buffer,
                           wgpu::IndexFormat::Uint32, /*index_count=*/6u,
                           InstancedMeshField::PassKind::kDeferred,
                           handle.operator->());

    // The shadow variant faces the LIGHT and cuts out against the same atlas,
    // so a distant model casts a model-shaped shadow rather than a sliver
    // (see foliage_impostor.wesl).
    auto shadow_handle = out->material_cache.GetOrCreate(
        ComposeMaterialCacheKey(entt::hashed_string{"lod_impostor_shadow"}.value(),
                                GeometryType::kInstancedMesh,
                                RenderPassType::kShadow, bucket),
        *out->impostor_factory, GeometryType::kInstancedMesh,
        MaterialPassType::kDeferred, RenderPassType::kShadow, params);
    if (!shadow_handle) {
      spdlog::error(
          "BuildInstancedLodField: impostor shadow material resolve failed at "
          "model {}",
          mi);
      return nullptr;
    }
    out->material_handles.push_back(shadow_handle);
    out->field->SetSubmeshShadow(mi, lod, /*submesh=*/0u,
                                 shadow_handle.operator->());
  }

  return out;
}

}  // namespace badlands
