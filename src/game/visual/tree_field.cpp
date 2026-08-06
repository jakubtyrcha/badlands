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
#include "game/geometry/leaf_voxelizer.hpp"  // VoxelizeLeafCards
#include "game/geometry/tree_generator.hpp"
#include "game/visual/foliage_voxel_config.hpp"  // SimplifyBarkForVoxelLod

namespace badlands {

namespace {

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

}  // namespace

TreeFieldModel BuildTreeFieldModel(const TreeOptions& options,
                                   float target_height_m) {
  TreeFieldModel out;
  out.options = options;

  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(options);
  BarkMeshStats bark_stats;
  out.bark_lod0 = GenerateTreeMesh(options, skeleton, &bark_stats);
  // One line per model, not per junction -- BuildForestModels runs this under
  // ParallelFor for every model in the forest. A non-zero `fallback` is the
  // signal that some branch could not be merged into its parent and still costs
  // its own mesh component, which is what caps how far bark can decimate.
  if (bark_stats.fallback > 0) {
    spdlog::info(
        "bark graft: seed {} -- {} junctions, {} stitched ({} shrunk), {} FELL BACK",
        options.seed, bark_stats.junctions, bark_stats.stitched, bark_stats.shrunk,
        bark_stats.fallback);
  } else {
    spdlog::debug("bark graft: seed {} -- {} junctions all stitched ({} shrunk)",
                  options.seed, bark_stats.junctions, bark_stats.shrunk);
  }

  const float bark_h = out.bark_lod0.local_bounds.max.y -
                       out.bark_lod0.local_bounds.min.y;
  const float native_h = std::max(bark_h, 0.001f);
  out.native_to_world_scale = target_height_m / native_h;
  out.target_height_m = target_height_m;

  const TexturedMeshResult leaves = GenerateLeafMesh(options, skeleton);
  out.leaf_lod_meshes.reserve(kFoliageVoxelWorldSizes.size());
  for (size_t lod = 0; lod < kFoliageVoxelWorldSizes.size(); ++lod) {
    LeafVoxelizeOptions voxel_opts;
    voxel_opts.cell_size = FoliageVoxelCellNativeM(lod, native_h);
    voxel_opts.position_jitter =
        FoliagePositionJitterForLod(lod, voxel_opts.position_jitter);
    out.leaf_lod_meshes.push_back(
        VoxelizeLeafCards(leaves.mesh, options.leaves.silhouette, voxel_opts));
  }

  const auto thresholds = FoliageLodThresholdsForHeight(target_height_m);
  out.lod_thresholds.assign(thresholds.begin(), thresholds.end());
  return out;
}

namespace {

// The bounds BuildTreeField publishes as TreeField::model_bounds. File-local:
// the union over every LOD is what an instance's GPU bounds sphere and the
// cell-cull padding want, and it is NOT what the foliage sampler should space
// by -- forest_renderer.cpp measures its own, finer silhouette for that. A
// public name here would invite exactly the wrong one.
TreeModelBounds ComputeTreeModelBounds(const TreeFieldModel& model) {
  TreeModelBounds bounds;
  bounds.bark_local_bounds = model.bark_lod0.local_bounds;

  // leaf_local_bounds = the UNION of every supplied LOD's own bounds (voxel
  // tets overscale past their source cards' AABB, and different LODs can have
  // different extents, so no single LOD's bounds alone would be conservative
  // for every LOD). Aabb::Union is a no-op for an Aabb::Empty()-sentinel
  // operand (min=+FLT_MAX/max=-FLT_MAX, see aabb.cpp), so unioning in an empty
  // LOD's bounds unguarded is safe -- matters for the known pine-preset gap
  // where one LOD's voxelization can legitimately be empty (see
  // leaf_voxelizer.hpp) while others aren't. has_leaves reflects whether ANY
  // supplied LOD has geometry.
  for (const TexturedMeshResult& m : model.leaf_lod_meshes) {
    if (m.mesh.vertex_count > 0) bounds.has_leaves = true;
    bounds.leaf_local_bounds = bounds.leaf_local_bounds.Union(m.local_bounds);
  }
  return bounds;
}

}  // namespace

std::unique_ptr<TreeField> BuildTreeField(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
    std::span<const TreeFieldModel> models, uint32_t capacity,
    const TreeFieldImpostor& impostor) {
  if (models.empty()) {
    spdlog::error("BuildTreeField: no models supplied");
    return nullptr;
  }
  // The impostor adds exactly one level on top of the voxel chain. Off unless
  // a valid atlas AND a placement per model were supplied -- a half-supplied
  // impostor silently rendering nothing at range would be worse than not
  // having one.
  const bool with_impostor = impostor.active(models.size());
  const uint32_t extra_lods = with_impostor ? 1u : 0u;

  for (size_t mi = 0; mi < models.size(); ++mi) {
    // Each model's RUNTIME LOD count: as many levels as the caller supplied
    // crown meshes for, plus the impostor if there is one, bounded by the
    // engine's compile-time cap.
    const size_t voxel_lods = models[mi].leaf_lod_meshes.size();
    const size_t lod_count = voxel_lods + extra_lods;
    if (lod_count == 0 || lod_count > GpuInstanceRenderer::kMaxLods) {
      spdlog::error(
          "BuildTreeField: model {} has leaf_lod_meshes.size()={} outside "
          "[1, kMaxLods={}]",
          mi, lod_count, GpuInstanceRenderer::kMaxLods);
      return nullptr;
    }
    // The model supplies the VOXEL chain's cutoffs; the impostor's own cutoff
    // is appended below rather than demanded from the caller, since a model has
    // no reason to know whether an impostor exists.
    //
    // The zero-voxel case is handled explicitly rather than folded into the
    // comparison: `voxel_lods - 1` is unsigned, so it would wrap to SIZE_MAX
    // and print as one. An impostor-only model is a legitimate one-level chain
    // and owes no cutoffs at all.
    const size_t expected_cutoffs = voxel_lods == 0 ? 0 : voxel_lods - 1;
    if (models[mi].lod_thresholds.size() != expected_cutoffs) {
      spdlog::error(
          "BuildTreeField: model {} has lod_thresholds.size()={} != {} (one "
          "cutoff between each adjacent pair of voxel levels)",
          mi, models[mi].lod_thresholds.size(), expected_cutoffs);
      return nullptr;
    }
  }

  // Effective cutoffs per model: the voxel chain's, plus the impostor's if it
  // is on. Height-scaled like every other level -- a taller tree is legitimately
  // legible from further away, so its switch distances are further too.
  std::vector<std::vector<float>> effective_thresholds(models.size());
  for (size_t mi = 0; mi < models.size(); ++mi) {
    effective_thresholds[mi] = models[mi].lod_thresholds;
    if (with_impostor) {
      effective_thresholds[mi].push_back(kFoliageImpostorThresholdPreviewM *
                                         models[mi].target_height_m /
                                         kFoliagePreviewHeight);
    }
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

  // --- Impostor factory + the shared unit quad (LOD4). ---
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
    tf->impostor_factory =
        BuildMaterialInstanceFactory(desc, device, queue, &pipeline_gen);
    if (!tf->impostor_factory) {
      spdlog::error("BuildTreeField: failed to build foliage_impostor factory");
      return nullptr;
    }

    // One quad for every model: the geometry is identical and only the
    // material differs. Position is unused -- the vertex stage builds the quad
    // from the instance transform -- so only uv carries information, and it IS
    // the local uv into the baked tile.
    const std::vector<float> quad = {
        // pos            uv          normal        tangent
        0, 0, 0,   0, 0,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   1, 0,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   0, 1,   0, 0, 1,   1, 0, 0, 1,
        0, 0, 0,   1, 1,   0, 0, 1,   1, 0, 0, 1,
    };
    const std::vector<uint32_t> quad_indices = {0, 1, 2, 2, 1, 3};
    tf->impostor_vertex_buffer = UploadVertexBuffer(device, quad);
    tf->impostor_index_buffer = UploadIndexBuffer(device, quad_indices);
    if (!tf->impostor_vertex_buffer || !tf->impostor_index_buffer) {
      spdlog::error("BuildTreeField: impostor quad upload failed");
      return nullptr;
    }
  }

  // One LOD chain per model: its runtime level count plus its own cutoffs,
  // padded out to the engine's fixed-width threshold array (entries past
  // lod_count-1 are never read -- see ModelLod). Models with different LOD
  // counts coexist; the chain is per model, not per field.
  std::vector<GpuInstanceRenderer::ModelLod> model_lods;
  model_lods.reserve(models.size());
  for (const TreeFieldModel& m : models) {
    GpuInstanceRenderer::ModelLod lod_chain;
    lod_chain.lod_count =
        static_cast<uint32_t>(m.leaf_lod_meshes.size()) + extra_lods;
    std::copy(effective_thresholds[model_lods.size()].begin(),
              effective_thresholds[model_lods.size()].end(),
              lod_chain.thresholds.begin());
    model_lods.push_back(lod_chain);
  }

  tf->field = std::make_unique<InstancedMeshField>(
      device, queue, pipeline_gen, capacity,
      static_cast<uint32_t>(models.size()),
      /*num_submeshes=*/2u, model_lods);
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

  // Each model's meshes are the CALLER's already-built geometry (see this
  // function's .hpp doc comment) -- generation is deterministic, so this used
  // to rebuild it internally, reproducing byte-identical results at the cost
  // of a second full generation pass on top of whatever the caller already
  // needed it for. The coarser LODs below simplify a COPY of the model's LOD0
  // bark through the shared SimplifyBarkForVoxelLod policy, the single-tree
  // path's exact pattern (model_viewer_view.cpp). Leaves have no such step:
  // `leaf_lod_meshes[lod]` is already the caller's final per-LOD mesh
  // (volumetric-foliage Phase 5).
  tf->model_bounds.resize(models.size());
  tf->native_to_world_scale.resize(models.size());
  tf->lod_buffers.resize(models.size());

  for (uint32_t model = 0; model < static_cast<uint32_t>(models.size());
       ++model) {
    const TreeFieldModel& src = models[model];
    const size_t lod_count = src.leaf_lod_meshes.size();

    tf->native_to_world_scale[model] = src.native_to_world_scale;

    tf->model_bounds[model] = ComputeTreeModelBounds(src);

    std::vector<TreeField::LodBuffers>& model_buffers = tf->lod_buffers[model];
    model_buffers.resize(lod_count);

    for (uint32_t lod = 0; lod < static_cast<uint32_t>(lod_count); ++lod) {
      StaticTexturedMeshComponent bark_mesh = src.bark_lod0.mesh;
      SimplifyBarkForVoxelLod(bark_mesh, lod);

      if (bark_mesh.indices.empty()) {
        spdlog::error("BuildTreeField: empty bark mesh at model {} lod {}",
                      model, lod);
        return nullptr;
      }

      const uint32_t bucket = GpuInstanceRenderer::BucketId(model, lod);
      TreeField::LodBuffers& buffers = model_buffers[lod];

      // --- Bark submesh (0, kDeferred + kShadow). ---
      buffers.bark_vertex_buffer =
          UploadVertexBuffer(device, bark_mesh.vertices);
      buffers.bark_index_buffer = UploadIndexBuffer(device, bark_mesh.indices);
      const uint32_t bark_index_count =
          static_cast<uint32_t>(bark_mesh.indices.size());

      InstanceParams bark_params;
      // instanced_gbuffer's group-0 texture slots (albedo@1, normal@3, arm@4)
      // are registered in material_requirements.cpp, so they carry the same
      // names normalmapped's do. `albedo` is left unset -- its "white" factory
      // default * `tint` below reproduces a flat solid bark color. Reused
      // as-is for the shadow-pass instance below (its bindings are declared
      // unconditionally in the WESL, not gated on shadow_pass -- see the
      // .wesl file) -- bucketId is what the shadow vertex path actually needs
      // (bucketBase[mat_params.bucketId]).
      bark_params.texture_overrides.push_back(DefaultTextureView{
          .param_name = "normal",
          .view = tf->bark_normal_view,
          .sampler = tf->bark_support_sampler,
          .type = TextureType::k2D,
      });
      bark_params.texture_overrides.push_back(DefaultTextureView{
          .param_name = "arm",
          .view = tf->bark_arm_view,
          .sampler = tf->bark_support_sampler,
          .type = TextureType::k2D,
      });
      bark_params.uniform_overrides["tint"] =
          MaterialParameterValue(glm::vec4(kTreeBarkColor, 1.0f));
      bark_params.uniform_overrides["bucketId"] =
          MaterialParameterValue(bucket);

      // The cache key is keyed on `bucket`, which already encodes
      // (model, lod) -- so every model gets its own material instances
      // without a second key dimension.
      entt::id_type bark_key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_bark"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kGBuffer, bucket);
      auto bark_handle = tf->material_cache.GetOrCreate(
          bark_key, *tf->bark_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kDeferred, RenderPassType::kGBuffer, bark_params);
      if (!bark_handle) {
        spdlog::error(
            "BuildTreeField: bark material resolve failed at model {} lod {}",
            model, lod);
        return nullptr;
      }
      tf->material_handles.push_back(bark_handle);
      tf->field->SetSubmesh(model, lod, /*submesh=*/0u,
                            buffers.bark_vertex_buffer,
                            buffers.bark_index_buffer,
                            wgpu::IndexFormat::Uint32, bark_index_count,
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
            "BuildTreeField: bark shadow material resolve failed at model {} "
            "lod {}",
            model, lod);
        return nullptr;
      }
      tf->material_handles.push_back(bark_shadow_handle);
      tf->field->SetSubmeshShadow(model, lod, /*submesh=*/0u,
                                  bark_shadow_handle.operator->());

      // --- Leaf submesh (1, kDeferred + kShadow), only for a LOD whose
      // supplied mesh actually has geometry -- a LOD can legally voxelize
      // empty (see leaf_voxelizer.hpp's known gap), in which case this slot
      // is simply left unconfigured: GpuInstanceRenderer::Draw skips zero-
      // index-count slots automatically, so this is safe (no dead draw, no
      // validation error) without any special-casing at draw time. A tree
      // with leaves at OTHER LODs hitting this at lod N still renders bare
      // there (fail-soft -- pine-at-dead-zone during dev must not brick the
      // viewer), but spdlog::warn's so the gap has a diagnostic instead of
      // silently popping the crown at that distance band. ---
      const StaticTexturedMeshComponent& leaf_mesh =
          src.leaf_lod_meshes[lod].mesh;
      if (leaf_mesh.vertex_count > 0 && !leaf_mesh.indices.empty()) {
        buffers.leaf_vertex_buffer =
            UploadVertexBuffer(device, leaf_mesh.vertices);
        buffers.leaf_index_buffer =
            UploadIndexBuffer(device, leaf_mesh.indices);
        const uint32_t leaf_index_count =
            static_cast<uint32_t>(leaf_mesh.indices.size());

        InstanceParams leaf_params;
        // voxel_foliage declares no textures at all (see the .wesl) -- no
        // texture_overrides needed. `params`: x = roughness, y = translucency
        // strength (shaders/material/voxel_foliage.wesl's MaterialParams).
        // Reused as-is for the shadow-pass instance below (voxel_foliage's
        // shadow_pass vertex stage still reads mat_params.bucketId).
        leaf_params.uniform_overrides["tint"] =
            MaterialParameterValue(glm::vec4(src.options.leaves.tint, 1.0f));
        leaf_params.uniform_overrides["params"] = MaterialParameterValue(
            glm::vec4(kBarkRoughness,
                      src.options.leaves.transmission_strength, 0.0f, 0.0f));
        leaf_params.uniform_overrides["bucketId"] =
            MaterialParameterValue(bucket);

        entt::id_type leaf_key = ComposeMaterialCacheKey(
            entt::hashed_string{"tree_leaf"}.value(),
            GeometryType::kInstancedMesh, RenderPassType::kGBuffer, bucket);
        auto leaf_handle = tf->material_cache.GetOrCreate(
            leaf_key, *tf->leaf_factory, GeometryType::kInstancedMesh,
            MaterialPassType::kDeferred, RenderPassType::kGBuffer,
            leaf_params);
        if (!leaf_handle) {
          spdlog::error(
              "BuildTreeField: leaf material resolve failed at model {} lod {}",
              model, lod);
          return nullptr;
        }
        tf->material_handles.push_back(leaf_handle);
        tf->field->SetSubmesh(model, lod, /*submesh=*/1u,
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
              "BuildTreeField: leaf shadow material resolve failed at model {} "
              "lod {}",
              model, lod);
          return nullptr;
        }
        tf->material_handles.push_back(leaf_shadow_handle);
        tf->field->SetSubmeshShadow(model, lod, /*submesh=*/1u,
                                    leaf_shadow_handle.operator->());
      } else if (tf->model_bounds[model].has_leaves) {
        spdlog::warn(
            "BuildTreeField: model {} lod {} supplied an empty leaf "
            "voxelization (tree has leaves at other LODs) -- rendering bare "
            "at this LOD",
            model, lod);
      }
    }

    // --- The impostor level. ---
    //
    // Submesh 0 only, and it carries the WHOLE tree: the atlas baked bark and
    // crown together, so there is no separate bark submesh here. Leaving
    // submesh 1 unconfigured is legal -- Draw skips zero-index-count slots.
    if (with_impostor) {
      const uint32_t lod = static_cast<uint32_t>(src.leaf_lod_meshes.size());
      const uint32_t bucket = GpuInstanceRenderer::BucketId(model, lod);
      const ImpostorPlacement& place = impostor.placement[model];

      InstanceParams params;
      if (!BindImpostorAtlas(*impostor.atlas, params)) return nullptr;
      params.uniform_overrides["placement"] = MaterialParameterValue(
          glm::vec4(place.local_center, place.radius));
      // params.w is the atlas LAYER, and the atlas is one layer per model, so
      // it is the model id. Carried as a float so the instanced and
      // non-instanced variants share one field list (see the shader).
      params.uniform_overrides["params"] = MaterialParameterValue(glm::vec4(
          kBarkRoughness, src.options.leaves.transmission_strength,
          kImpostorAlphaCutoff, static_cast<float>(model)));
      params.uniform_overrides["bucketId"] = MaterialParameterValue(bucket);

      entt::id_type key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_impostor"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kGBuffer, bucket);
      auto handle = tf->material_cache.GetOrCreate(
          key, *tf->impostor_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kDeferred, RenderPassType::kGBuffer, params);
      if (!handle) {
        spdlog::error("BuildTreeField: impostor material resolve failed at "
                      "model {}", model);
        return nullptr;
      }
      tf->material_handles.push_back(handle);
      tf->field->SetSubmesh(model, lod, /*submesh=*/0u,
                            tf->impostor_vertex_buffer,
                            tf->impostor_index_buffer,
                            wgpu::IndexFormat::Uint32, /*index_count=*/6u,
                            InstancedMeshField::PassKind::kDeferred,
                            handle.operator->());

      // The shadow variant faces the LIGHT and cuts out against the same
      // atlas, so a distant tree casts a tree-shaped shadow rather than a
      // sliver (see foliage_impostor.wesl).
      entt::id_type shadow_key = ComposeMaterialCacheKey(
          entt::hashed_string{"tree_impostor_shadow"}.value(),
          GeometryType::kInstancedMesh, RenderPassType::kShadow, bucket);
      auto shadow_handle = tf->material_cache.GetOrCreate(
          shadow_key, *tf->impostor_factory, GeometryType::kInstancedMesh,
          MaterialPassType::kDeferred, RenderPassType::kShadow, params);
      if (!shadow_handle) {
        spdlog::error("BuildTreeField: impostor shadow material resolve failed "
                      "at model {}", model);
        return nullptr;
      }
      tf->material_handles.push_back(shadow_handle);
      tf->field->SetSubmeshShadow(model, lod, /*submesh=*/0u,
                                  shadow_handle.operator->());
    }
  }

  return tf;
}

}  // namespace badlands
