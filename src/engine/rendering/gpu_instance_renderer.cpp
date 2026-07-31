#include "engine/rendering/gpu_instance_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/frustum.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

namespace {

// Mirrors `CullConfig` in the three compute shaders byte-for-byte (144 bytes:
// 6 planes + camera pos + lod thresholds + counts, all 16-byte aligned).
struct CullConfigData {
  std::array<glm::vec4, 6> planes{};
  glm::vec4 camera_world_pos{0.0f};
  glm::vec4 lod_thresholds{0.0f};  // x=t0, y=t1 (kMaxLods-1); z,w unused
  glm::uvec4 counts{0u};  // x=instanceCount, y=numBuckets, z=compactedCapacity, w=numSubmeshes
};
static_assert(sizeof(CullConfigData) == 144);

// Mirrors `IndirectArgs` in the shaders (and the standard DrawIndexedIndirect
// args layout) byte-for-byte.
struct IndirectArgsData {
  uint32_t index_count = 0;
  uint32_t instance_count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  uint32_t first_instance = 0;
};
static_assert(sizeof(IndirectArgsData) == GpuInstanceRenderer::kArgsStride);

wgpu::Buffer MakeBuffer(wgpu::Device device, uint64_t size,
                        wgpu::BufferUsage usage) {
  wgpu::BufferDescriptor bd{};
  bd.size = std::max<uint64_t>(size, 4);
  bd.usage = usage;
  return device.CreateBuffer(&bd);
}

wgpu::BindGroupEntry StorageEntry(uint32_t binding, wgpu::Buffer buffer) {
  wgpu::BindGroupEntry e{};
  e.binding = binding;
  e.buffer = buffer;
  e.size = WGPU_WHOLE_SIZE;
  return e;
}

}  // namespace

GpuInstanceRenderer::GpuInstanceRenderer(
    wgpu::Device device, wgpu::Queue queue,
    GpuPipelineGenerator& pipeline_generator, uint32_t capacity,
    uint32_t num_models, std::array<float, kMaxLods - 1> lod_thresholds,
    uint32_t num_submeshes)
    : device_(device),
      queue_(queue),
      capacity_(capacity),
      num_models_(std::max(1u, num_models)),
      num_submeshes_(std::max(1u, num_submeshes)),
      lod_thresholds_(lod_thresholds) {
  num_buckets_ = num_models_ * kMaxLods;

  classify_pipeline_ =
      pipeline_generator.GetComputePipeline("compute/instance_classify");
  scan_pipeline_ =
      pipeline_generator.GetComputePipeline("compute/instance_scan");
  scatter_pipeline_ =
      pipeline_generator.GetComputePipeline("compute/instance_scatter");
  if (!IsValid()) {
    spdlog::error(
        "GpuInstanceRenderer: failed to compile the cull compute pipelines");
    classify_pipeline_ = nullptr;
    scan_pipeline_ = nullptr;
    scatter_pipeline_ = nullptr;
    return;
  }

  // Worst-case compacted size: every instance survives (capacity slots). The
  // scan packs bucket bases with a tight exclusive prefix-sum (no padding —
  // see instance_scan.wesl), so no extra headroom is needed. The scatter
  // clamps writes to this capacity regardless.
  compacted_capacity_ = capacity_;

  config_buffer_ = MakeBuffer(device_, sizeof(CullConfigData),
                              wgpu::BufferUsage::Uniform |
                                  wgpu::BufferUsage::CopyDst);
  instance_buffer_ =
      MakeBuffer(device_, uint64_t{capacity_} * sizeof(InstanceInput),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
  per_instance_bucket_buffer_ = MakeBuffer(
      device_, uint64_t{capacity_} * sizeof(uint32_t), wgpu::BufferUsage::Storage);
  bucket_count_buffer_ =
      MakeBuffer(device_, uint64_t{num_buckets_} * sizeof(uint32_t),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                     wgpu::BufferUsage::CopySrc);
  bucket_base_buffer_ =
      MakeBuffer(device_, uint64_t{num_buckets_} * sizeof(uint32_t),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  write_cursor_buffer_ = MakeBuffer(
      device_, uint64_t{num_buckets_} * sizeof(uint32_t), wgpu::BufferUsage::Storage);
  compacted_buffer_ =
      MakeBuffer(device_, uint64_t{compacted_capacity_} * sizeof(glm::mat4),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  const uint32_t num_slots = num_buckets_ * num_submeshes_;
  args_buffer_ =
      MakeBuffer(device_, uint64_t{num_slots} * sizeof(IndirectArgsData),
                 wgpu::BufferUsage::Indirect | wgpu::BufferUsage::Storage |
                     wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);

  // Zero the main set's indirect args (index/instance/first/base all 0), one
  // slot per (bucket, submesh). SetBucketSubmesh fills in each configured
  // slot's indexCount; the scan fills instanceCount (broadcast to every
  // submesh slot of a bucket) each Cull() call. The SHADOW set's args buffer
  // is zeroed the same way, lazily, by EnsureShadowCullResources() below --
  // it isn't allocated here at all (see that method + the shadow buffer
  // fields' declaration comment in the header for why).
  std::vector<IndirectArgsData> zero_args(num_slots);
  queue_.WriteBuffer(args_buffer_, 0, zero_args.data(),
                     zero_args.size() * sizeof(IndirectArgsData));

  submesh_meshes_.resize(num_slots);

  // Bind groups reference stable buffer OBJECTS (contents overwritten in place),
  // so build them once. All three passes bind only group 0. Each pass declares
  // exactly the bindings it uses at contiguous binding numbers (0,1,2,…); the
  // buffer bound at each slot is what that pass reads/writes.
  {  // classify: config, instances, perInstanceBucket, bucketCount
    std::array<wgpu::BindGroupEntry, 4> e{
        StorageEntry(0, config_buffer_), StorageEntry(1, instance_buffer_),
        StorageEntry(2, per_instance_bucket_buffer_),
        StorageEntry(3, bucket_count_buffer_)};
    classify_bind_group_ =
        CreateComputeBindGroup(device_, *classify_pipeline_, e);
  }
  {  // scan: config, bucketCount, bucketBase, writeCursor, indirectArgs
    std::array<wgpu::BindGroupEntry, 5> e{
        StorageEntry(0, config_buffer_), StorageEntry(1, bucket_count_buffer_),
        StorageEntry(2, bucket_base_buffer_),
        StorageEntry(3, write_cursor_buffer_), StorageEntry(4, args_buffer_)};
    scan_bind_group_ = CreateComputeBindGroup(device_, *scan_pipeline_, e);
  }
  {  // scatter: config, instances, perInstanceBucket, bucketBase, writeCursor,
     // compacted
    std::array<wgpu::BindGroupEntry, 6> e{
        StorageEntry(0, config_buffer_), StorageEntry(1, instance_buffer_),
        StorageEntry(2, per_instance_bucket_buffer_),
        StorageEntry(3, bucket_base_buffer_),
        StorageEntry(4, write_cursor_buffer_),
        StorageEntry(5, compacted_buffer_)};
    scatter_bind_group_ = CreateComputeBindGroup(device_, *scatter_pipeline_, e);
  }
}

void GpuInstanceRenderer::EnsureShadowCullResources() {
  if (shadow_resources_created_ || !IsValid()) {
    return;
  }

  // Second buffer set for CullShadow()'s light-frustum cull -- same sizes,
  // same usage flags as the main set the constructor built above
  // (instance_buffer_ is the only one NOT duplicated: both culls classify the
  // same static per-instance input). See GpuInstanceRenderer::CullShadow's
  // header comment for why a dedicated set exists instead of a second
  // same-frame call sharing the main one.
  config_buffer_shadow_ = MakeBuffer(device_, sizeof(CullConfigData),
                                     wgpu::BufferUsage::Uniform |
                                         wgpu::BufferUsage::CopyDst);
  per_instance_bucket_buffer_shadow_ = MakeBuffer(
      device_, uint64_t{capacity_} * sizeof(uint32_t), wgpu::BufferUsage::Storage);
  bucket_count_buffer_shadow_ =
      MakeBuffer(device_, uint64_t{num_buckets_} * sizeof(uint32_t),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                     wgpu::BufferUsage::CopySrc);
  bucket_base_buffer_shadow_ =
      MakeBuffer(device_, uint64_t{num_buckets_} * sizeof(uint32_t),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  write_cursor_buffer_shadow_ = MakeBuffer(
      device_, uint64_t{num_buckets_} * sizeof(uint32_t), wgpu::BufferUsage::Storage);
  compacted_buffer_shadow_ =
      MakeBuffer(device_, uint64_t{compacted_capacity_} * sizeof(glm::mat4),
                 wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  const uint32_t num_slots = static_cast<uint32_t>(submesh_meshes_.size());
  args_buffer_shadow_ =
      MakeBuffer(device_, uint64_t{num_slots} * sizeof(IndirectArgsData),
                 wgpu::BufferUsage::Indirect | wgpu::BufferUsage::Storage |
                     wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);

  // Zero the shadow set's indirect args first (index/instance/first/base all
  // 0), THEN replay every already-configured slot's geometry fields over it
  // below -- mirrors the constructor's main-set zero-fill, but this set can
  // now be created well after SetBucketSubmesh calls the main set already
  // saw, so a fresh zero-fill alone would silently lose those slots'
  // indexCount until their next SetBucketSubmesh call (which may never come
  // again for a field built once at startup).
  std::vector<IndirectArgsData> zero_args(num_slots);
  queue_.WriteBuffer(args_buffer_shadow_, 0, zero_args.data(),
                     zero_args.size() * sizeof(IndirectArgsData));

  // Same trio as the constructor's main-set bind groups, over the shadow set
  // (instance_buffer_ shared with the main classify/scatter bind groups).
  {
    std::array<wgpu::BindGroupEntry, 4> e{
        StorageEntry(0, config_buffer_shadow_), StorageEntry(1, instance_buffer_),
        StorageEntry(2, per_instance_bucket_buffer_shadow_),
        StorageEntry(3, bucket_count_buffer_shadow_)};
    classify_bind_group_shadow_ =
        CreateComputeBindGroup(device_, *classify_pipeline_, e);
  }
  {
    std::array<wgpu::BindGroupEntry, 5> e{
        StorageEntry(0, config_buffer_shadow_),
        StorageEntry(1, bucket_count_buffer_shadow_),
        StorageEntry(2, bucket_base_buffer_shadow_),
        StorageEntry(3, write_cursor_buffer_shadow_),
        StorageEntry(4, args_buffer_shadow_)};
    scan_bind_group_shadow_ = CreateComputeBindGroup(device_, *scan_pipeline_, e);
  }
  {
    std::array<wgpu::BindGroupEntry, 6> e{
        StorageEntry(0, config_buffer_shadow_), StorageEntry(1, instance_buffer_),
        StorageEntry(2, per_instance_bucket_buffer_shadow_),
        StorageEntry(3, bucket_base_buffer_shadow_),
        StorageEntry(4, write_cursor_buffer_shadow_),
        StorageEntry(5, compacted_buffer_shadow_)};
    scatter_bind_group_shadow_ =
        CreateComputeBindGroup(device_, *scatter_pipeline_, e);
  }

  // REPLAY: mirrors SetBucketSubmesh's own two targeted args writes (see its
  // .cpp comment for the exact field layout) for every slot already
  // configured before this resource creation -- a slot's SetBucketSubmesh
  // call could not mirror into args_buffer_shadow_ back when it ran (this
  // set didn't exist yet), so this is the only chance for that slot's
  // indexCount to reach the shadow args buffer short of a second
  // SetBucketSubmesh call.
  for (uint32_t slot = 0; slot < num_slots; ++slot) {
    const SubmeshMesh& mesh = submesh_meshes_[slot];
    const uint64_t base = uint64_t{slot} * sizeof(IndirectArgsData);
    IndirectArgsData args{};
    args.index_count = mesh.index_count;
    queue_.WriteBuffer(args_buffer_shadow_, base, &args.index_count,
                       sizeof(uint32_t));
    queue_.WriteBuffer(args_buffer_shadow_, base + 8, &args.first_index,
                       3 * sizeof(uint32_t));
  }

  shadow_resources_created_ = true;
}

void GpuInstanceRenderer::EnsureShadowCull() {
  if (!IsValid()) {
    return;
  }
  EnsureShadowCullResources();
}

void GpuInstanceRenderer::UploadInstances(
    std::span<const InstanceInput> instances) {
  if (!IsValid()) {
    return;
  }
  uint32_t count = static_cast<uint32_t>(instances.size());
  if (count > capacity_) {
    spdlog::error(
        "GpuInstanceRenderer::UploadInstances: {} instances exceeds capacity {} "
        "— truncating",
        instances.size(), capacity_);
    count = capacity_;
  }
  instance_count_ = count;
  if (count > 0) {
    queue_.WriteBuffer(instance_buffer_, 0, instances.data(),
                       uint64_t{count} * sizeof(InstanceInput));
  }
}

void GpuInstanceRenderer::SetBucketSubmesh(uint32_t bucket, uint32_t submesh,
                                           wgpu::Buffer vertex_buffer,
                                           wgpu::Buffer index_buffer,
                                           wgpu::IndexFormat index_format,
                                           uint32_t index_count) {
  if (!IsValid() || bucket >= num_buckets_ || submesh >= num_submeshes_) {
    spdlog::error(
        "GpuInstanceRenderer::SetBucketSubmesh: (bucket {}, submesh {}) out of "
        "range ({}, {})",
        bucket, submesh, num_buckets_, num_submeshes_);
    return;
  }
  const uint32_t slot = bucket * num_submeshes_ + submesh;
  submesh_meshes_[slot] = {vertex_buffer, index_buffer, index_format,
                           index_count};

  // Pre-fill this slot's indirect-args GEOMETRY fields only, WITHOUT touching
  // instanceCount@4: the scan publishes that every Cull()/CullShadow() call
  // (broadcast to every submesh slot of the bucket), and SetBucketSubmesh may
  // be called AFTER Cull() (e.g. to swap a slot's mesh) -- writing the whole
  // 20-byte struct here would zero the slot's GPU-published survivor count.
  // Two targeted writes straddle the instanceCount slot (struct layout:
  // indexCount@0, instanceCount@4, firstIndex@8, baseVertex@12,
  // firstInstance@16):
  //   [0..4)   indexCount
  //   [8..20)  firstIndex + baseVertex + firstInstance (all 0)
  // Mirrored into the main args buffer unconditionally; mirrored into the
  // SHADOW args buffer too, but ONLY if the shadow buffer set already exists
  // (shadow_resources_created_ -- see EnsureShadowCullResources(): it's
  // allocated lazily, so a slot configured before that creation would target
  // a null args_buffer_shadow_ here otherwise). A slot configured before the
  // shadow set exists still gets its geometry there eventually --
  // EnsureShadowCullResources() REPLAYS every submesh_meshes_ entry
  // (including this one, updated just above) once it creates the buffer.
  IndirectArgsData args{};
  args.index_count = index_count;
  const uint64_t base = uint64_t{slot} * sizeof(IndirectArgsData);
  queue_.WriteBuffer(args_buffer_, base, &args.index_count, sizeof(uint32_t));
  queue_.WriteBuffer(args_buffer_, base + 8, &args.first_index,
                     3 * sizeof(uint32_t));
  if (shadow_resources_created_) {
    queue_.WriteBuffer(args_buffer_shadow_, base, &args.index_count,
                       sizeof(uint32_t));
    queue_.WriteBuffer(args_buffer_shadow_, base + 8, &args.first_index,
                       3 * sizeof(uint32_t));
  }
}

void GpuInstanceRenderer::Cull(FrameContext& frame, const Camera& camera) {
  if (!IsValid()) {
    return;
  }
  const Frustum frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());
  CullInternal(frame, frustum, camera.GetPosition(), config_buffer_,
              bucket_count_buffer_, classify_bind_group_, scan_bind_group_,
              scatter_bind_group_);
}

void GpuInstanceRenderer::CullShadow(FrameContext& frame, const Camera& camera,
                                     const glm::mat4& light_view_proj) {
  if (!IsValid()) {
    return;
  }
  // Lazily provision the shadow buffer set/bind groups (a no-op if
  // EnsureShadowCull() or an earlier CullShadow() already did) before this
  // dispatch reads/writes them.
  EnsureShadowCullResources();
  // Frustum from the LIGHT's view-proj, but camera_world_pos/LOD thresholds
  // still from `camera` -- see this method's header comment.
  const Frustum frustum = Frustum::FromViewProj(light_view_proj);
  CullInternal(frame, frustum, camera.GetPosition(), config_buffer_shadow_,
              bucket_count_buffer_shadow_, classify_bind_group_shadow_,
              scan_bind_group_shadow_, scatter_bind_group_shadow_);
}

void GpuInstanceRenderer::CullInternal(
    FrameContext& frame, const Frustum& frustum, glm::vec3 camera_world_pos,
    wgpu::Buffer config_buffer, wgpu::Buffer bucket_count_buffer,
    wgpu::BindGroup classify_bind_group, wgpu::BindGroup scan_bind_group,
    wgpu::BindGroup scatter_bind_group) {
  CullConfigData config{};
  for (int i = 0; i < 6; ++i) {
    config.planes[static_cast<size_t>(i)] = frustum.planes[i];
  }
  config.camera_world_pos = glm::vec4(camera_world_pos, 0.0f);
  config.lod_thresholds = glm::vec4(0.0f);
  for (uint32_t i = 0; i < kMaxLods - 1; ++i) {
    config.lod_thresholds[static_cast<int>(i)] = lod_thresholds_[i];
  }
  config.counts = glm::uvec4(instance_count_, num_buckets_, compacted_capacity_,
                             num_submeshes_);
  queue_.WriteBuffer(config_buffer, 0, &config, sizeof(config));

  // Clear the per-bucket counts before classify atomicAdds into them. The scan
  // resets writeCursor itself; per_instance_bucket entries beyond
  // instance_count_ are never read (both classify and scatter guard i >= count).
  frame.GetEncoder().ClearBuffer(bucket_count_buffer, 0,
                                 uint64_t{num_buckets_} * sizeof(uint32_t));

  const uint32_t classify_wg =
      std::max(1u, classify_pipeline_->workgroup_size[0]);
  const uint32_t classify_dispatch =
      (instance_count_ + classify_wg - 1) / classify_wg;
  const uint32_t scatter_wg =
      std::max(1u, scatter_pipeline_->workgroup_size[0]);
  const uint32_t scatter_dispatch =
      (instance_count_ + scatter_wg - 1) / scatter_wg;

  // Three separate compute passes on the same encoder — WebGPU inserts the
  // read-after-write barriers between passes automatically (classify writes the
  // counts the scan reads; the scan writes the bases the scatter reads).
  if (classify_dispatch > 0) {
    wgpu::ComputePassEncoder pass = frame.BeginComputePass();
    pass.SetPipeline(classify_pipeline_->pipeline);
    pass.SetBindGroup(0, classify_bind_group, 0, nullptr);
    pass.DispatchWorkgroups(classify_dispatch, 1, 1);
    pass.End();
  }
  {  // scan: always runs (publishes bases + zeroes instanceCounts even for 0)
    wgpu::ComputePassEncoder pass = frame.BeginComputePass();
    pass.SetPipeline(scan_pipeline_->pipeline);
    pass.SetBindGroup(0, scan_bind_group, 0, nullptr);
    pass.DispatchWorkgroups(1, 1, 1);
    pass.End();
  }
  if (scatter_dispatch > 0) {
    wgpu::ComputePassEncoder pass = frame.BeginComputePass();
    pass.SetPipeline(scatter_pipeline_->pipeline);
    pass.SetBindGroup(0, scatter_bind_group, 0, nullptr);
    pass.DispatchWorkgroups(scatter_dispatch, 1, 1);
    pass.End();
  }
}

void GpuInstanceRenderer::Draw(
    RenderPassContext& pass, FrameContext& frame,
    const BucketSubmeshMaterialFn& material_for_bucket_submesh,
    CullSet cull_set) const {
  if (!IsValid()) {
    return;
  }
  const bool shadow = cull_set == CullSet::kShadow;
  // A field with no shadow-casting submesh never triggers CullShadow() or
  // EnsureShadowCull() (see e.g. scene_renderer.cpp's HasPass(kShadow) gate
  // around its CullShadow() call), so the shadow buffer set can genuinely
  // never exist by the time Draw(..., kShadow) is called -- which itself is
  // NOT gated the same way (see this method's header comment). Bail before
  // touching any shadow_*_ buffer: they're null pre-creation, and binding/
  // indirect-drawing against a null buffer is a Dawn validation error, not
  // the safe no-op this call must be.
  if (shadow && !shadow_resources_created_) {
    return;
  }
  const wgpu::Buffer& compacted = shadow ? compacted_buffer_shadow_ : compacted_buffer_;
  const wgpu::Buffer& bucket_base = shadow ? bucket_base_buffer_shadow_ : bucket_base_buffer_;
  const wgpu::Buffer& args = shadow ? args_buffer_shadow_ : args_buffer_;
  for (uint32_t bucket = 0; bucket < num_buckets_; ++bucket) {
    for (uint32_t submesh = 0; submesh < num_submeshes_; ++submesh) {
      const uint32_t slot = bucket * num_submeshes_ + submesh;
      const SubmeshMesh& mesh = submesh_meshes_[slot];
      if (!mesh.vertex_buffer || !mesh.index_buffer || mesh.index_count == 0) {
        continue;  // slot has no geometry configured
      }
      RenderingMaterialInstance* material =
          material_for_bucket_submesh(bucket, submesh);
      if (!material) {
        continue;  // caller opted out of this slot
      }
      if (!material->BindInstanceData(pass, frame, compacted, bucket_base)) {
        continue;  // not an instanced material
      }
      pass.SetVertexBuffer(0, mesh.vertex_buffer);
      pass.SetIndexBuffer(mesh.index_buffer, mesh.index_format);
      // Slots with 0 survivors draw nothing (indirect instanceCount == 0).
      pass.DrawIndexedIndirect(args, uint64_t{slot} * sizeof(IndirectArgsData));
    }
  }
}

}  // namespace badlands
