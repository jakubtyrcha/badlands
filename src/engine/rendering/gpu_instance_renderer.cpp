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
  glm::uvec4 counts{0u};  // x=instanceCount, y=numBuckets, z=compactedCapacity
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
    uint32_t num_models, std::array<float, kMaxLods - 1> lod_thresholds)
    : device_(device),
      queue_(queue),
      capacity_(capacity),
      num_models_(std::max(1u, num_models)),
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
  args_buffer_ =
      MakeBuffer(device_, uint64_t{num_buckets_} * sizeof(IndirectArgsData),
                 wgpu::BufferUsage::Indirect | wgpu::BufferUsage::Storage |
                     wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);

  // Zero the indirect args (index/instance/first/base all 0). SetBucketMesh
  // fills in each configured bucket's indexCount; the scan fills instanceCount.
  std::vector<IndirectArgsData> zero_args(num_buckets_);
  queue_.WriteBuffer(args_buffer_, 0, zero_args.data(),
                     zero_args.size() * sizeof(IndirectArgsData));

  bucket_meshes_.resize(num_buckets_);

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

void GpuInstanceRenderer::SetBucketMesh(uint32_t bucket,
                                        wgpu::Buffer vertex_buffer,
                                        wgpu::Buffer index_buffer,
                                        wgpu::IndexFormat index_format,
                                        uint32_t index_count) {
  if (!IsValid() || bucket >= num_buckets_) {
    spdlog::error("GpuInstanceRenderer::SetBucketMesh: bucket {} out of range {}",
                  bucket, num_buckets_);
    return;
  }
  bucket_meshes_[bucket] = {vertex_buffer, index_buffer, index_format,
                            index_count};

  // Pre-fill this bucket's indirect-args GEOMETRY fields only, WITHOUT touching
  // instanceCount@4: the scan publishes that every Cull(), and SetBucketMesh may
  // be called AFTER Cull() (e.g. to swap a bucket's mesh) -- writing the whole
  // 20-byte struct here would zero the bucket's GPU-published survivor count.
  // Two targeted writes straddle the instanceCount slot (struct layout:
  // indexCount@0, instanceCount@4, firstIndex@8, baseVertex@12, firstInstance@16):
  //   [0..4)   indexCount
  //   [8..20)  firstIndex + baseVertex + firstInstance (all 0)
  IndirectArgsData args{};
  args.index_count = index_count;
  const uint64_t base = uint64_t{bucket} * sizeof(IndirectArgsData);
  queue_.WriteBuffer(args_buffer_, base, &args.index_count, sizeof(uint32_t));
  queue_.WriteBuffer(args_buffer_, base + 8, &args.first_index,
                     3 * sizeof(uint32_t));
}

void GpuInstanceRenderer::Cull(FrameContext& frame, const Camera& camera) {
  if (!IsValid()) {
    return;
  }

  const Frustum frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());
  CullConfigData config{};
  for (int i = 0; i < 6; ++i) {
    config.planes[static_cast<size_t>(i)] = frustum.planes[i];
  }
  config.camera_world_pos = glm::vec4(camera.GetPosition(), 0.0f);
  config.lod_thresholds = glm::vec4(0.0f);
  for (uint32_t i = 0; i < kMaxLods - 1; ++i) {
    config.lod_thresholds[static_cast<int>(i)] = lod_thresholds_[i];
  }
  config.counts =
      glm::uvec4(instance_count_, num_buckets_, compacted_capacity_, 0u);
  queue_.WriteBuffer(config_buffer_, 0, &config, sizeof(config));

  // Clear the per-bucket counts before classify atomicAdds into them. The scan
  // resets writeCursor itself; per_instance_bucket entries beyond
  // instance_count_ are never read (both classify and scatter guard i >= count).
  frame.GetEncoder().ClearBuffer(bucket_count_buffer_, 0,
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
    pass.SetBindGroup(0, classify_bind_group_, 0, nullptr);
    pass.DispatchWorkgroups(classify_dispatch, 1, 1);
    pass.End();
  }
  {  // scan: always runs (publishes bases + zeroes instanceCounts even for 0)
    wgpu::ComputePassEncoder pass = frame.BeginComputePass();
    pass.SetPipeline(scan_pipeline_->pipeline);
    pass.SetBindGroup(0, scan_bind_group_, 0, nullptr);
    pass.DispatchWorkgroups(1, 1, 1);
    pass.End();
  }
  if (scatter_dispatch > 0) {
    wgpu::ComputePassEncoder pass = frame.BeginComputePass();
    pass.SetPipeline(scatter_pipeline_->pipeline);
    pass.SetBindGroup(0, scatter_bind_group_, 0, nullptr);
    pass.DispatchWorkgroups(scatter_dispatch, 1, 1);
    pass.End();
  }
}

void GpuInstanceRenderer::Draw(
    RenderPassContext& pass, FrameContext& frame,
    const BucketMaterialFn& material_for_bucket) const {
  if (!IsValid()) {
    return;
  }
  for (uint32_t bucket = 0; bucket < num_buckets_; ++bucket) {
    const BucketMesh& mesh = bucket_meshes_[bucket];
    if (!mesh.vertex_buffer || !mesh.index_buffer || mesh.index_count == 0) {
      continue;  // bucket has no geometry configured
    }
    RenderingMaterialInstance* material = material_for_bucket(bucket);
    if (!material) {
      continue;  // caller opted out of this bucket
    }
    if (!material->BindInstanceData(pass, frame, compacted_buffer_,
                                    bucket_base_buffer_)) {
      continue;  // not an instanced material
    }
    pass.SetVertexBuffer(0, mesh.vertex_buffer);
    pass.SetIndexBuffer(mesh.index_buffer, mesh.index_format);
    // Buckets with 0 survivors draw nothing (indirect instanceCount == 0).
    pass.DrawIndexedIndirect(args_buffer_,
                             uint64_t{bucket} * sizeof(IndirectArgsData));
  }
}

}  // namespace badlands
