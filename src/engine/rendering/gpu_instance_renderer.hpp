#pragma once

// GPU-driven instanced rendering, Phase D: multi-bucket LOD selection +
// prefix-sum compaction feeding ONE indirect draw per (model,lod) bucket.
// Engine-generic — no scene/ECS/material-cache assumptions. Owns the
// instance-input buffer, the per-bucket bookkeeping buffers (counts, bases,
// cursors), the single global compacted-transform buffer, and the per-bucket
// indirect-args buffer, plus the three cull/scan/scatter compute pipelines.
//
// Generalizes Phase C's single-bucket cull: instead of one atomic-append into
// one buffer + one draw, a 3-pass compute (classify+count → prefix-sum scan →
// scatter) routes each surviving instance into `bucket = modelId*kMaxLods +
// lod(distance)`, compacts survivors into disjoint prefix-summed slices of one
// global buffer, and the CPU issues one DrawIndexedIndirect per bucket. The
// per-bucket base offset is GPU-computed (the scan), so the draw's VERTEX shader
// reads it: `compacted[bucketBase[bucketId] + instance_index]` with `bucketId` a
// CPU-known per-draw constant. No per-frame readback of the offsets. Phase C's
// single bucket is the trivial 1-model/1-lod case (`bucketBase=[0]`).
//
// The caller resolves and binds whatever RenderingMaterialInstance it wants per
// bucket (e.g. via MaterialInstanceCache — see the tests) and hands them to
// `Draw()` through a per-bucket callback; this class never reaches into a
// material cache, scene, or ECS itself.
//
// Sequencing: `Cull()` dispatches THREE compute passes on `frame`'s encoder and
// MUST run before any render pass is begun on that same encoder (compute and
// render passes cannot be active simultaneously on one encoder). `Draw()` runs
// inside an already-active render pass, after `Cull()` ran earlier on the same
// encoder; the compute→render buffer dependency is handled automatically by the
// encoder (no manual barrier needed in WebGPU).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/core/camera.hpp"

namespace badlands {

class FrameContext;
class RenderPassContext;
class GpuPipelineGenerator;
class RenderingMaterialInstance;
struct CompiledComputePipeline;

class GpuInstanceRenderer {
 public:
  // Level-of-detail count per model. `bucket = modelId*kMaxLods + lod`, so this
  // MUST match `kMaxLods` in shaders/compute/instance_classify.wesl. kMaxLods-1
  // distance thresholds separate the LODs.
  static constexpr uint32_t kMaxLods = 3;

  // Static per-instance input, uploaded once (or replaced wholesale via
  // UploadInstances). Mirrors `InstanceData` in the compute shaders
  // byte-for-byte (96 bytes, no padding gaps).
  struct InstanceInput {
    glm::mat4 transform{1.0f};      // world transform
    glm::vec4 bounds_sphere{0.0f};  // xyz = world center, w = radius
    glm::uvec4 model_info{0u};      // x = model id (selects the bucket group)
  };

  // `capacity` upper-bounds the instance-input set. `num_models` is the number
  // of distinct model ids (buckets = num_models*kMaxLods). `lod_thresholds` are
  // the kMaxLods-1 ascending distance cutoffs (LOD0 for dist < [0], LOD1 for
  // dist < [1], … coarsest otherwise). Compiles the three compute pipelines
  // immediately; IsValid() reports whether that succeeded.
  GpuInstanceRenderer(wgpu::Device device, wgpu::Queue queue,
                      GpuPipelineGenerator& pipeline_generator,
                      uint32_t capacity, uint32_t num_models,
                      std::array<float, kMaxLods - 1> lod_thresholds);

  bool IsValid() const {
    return classify_pipeline_ != nullptr && scan_pipeline_ != nullptr &&
           scatter_pipeline_ != nullptr;
  }

  uint32_t GetCapacity() const { return capacity_; }
  uint32_t GetNumModels() const { return num_models_; }
  uint32_t GetNumBuckets() const { return num_buckets_; }
  // The bucket id a given (model, lod) routes to — the CPU-known per-draw
  // constant the vertex shader uses to look up its base offset.
  static uint32_t BucketId(uint32_t model_id, uint32_t lod) {
    return model_id * kMaxLods + lod;
  }

  // Replaces the static instance-input set wholesale. `instances.size()` must be
  // <= capacity (larger inputs are truncated, logged as an error).
  void UploadInstances(std::span<const InstanceInput> instances);

  // Configure a bucket's geometry (the mesh drawn per surviving instance in that
  // bucket) and pre-fill its indirect-args indexCount (firstIndex/baseVertex
  // stay 0 -- each bucket binds its own index buffer, so no shared-buffer
  // offset is needed). The vertex layout must match whatever instanced
  // material `Draw()` later resolves for the bucket. Buckets left unconfigured
  // (index_count 0) are skipped by Draw(). `bucket` must be < GetNumBuckets().
  void SetBucketMesh(uint32_t bucket, wgpu::Buffer vertex_buffer,
                     wgpu::Buffer index_buffer, wgpu::IndexFormat index_format,
                     uint32_t index_count);

  // Clears the per-bucket counts, derives the world-space frustum + camera
  // position from `camera`, uploads the cull config (frustum + thresholds +
  // counts), and dispatches the three compute passes (classify+count → scan →
  // scatter). See the class comment for the encoder-sequencing requirement
  // (must precede BeginRenderPass).
  void Cull(FrameContext& frame, const Camera& camera);

  // Per-bucket material resolver: given a bucket id, return the material
  // instance to render that bucket with — already bound (pipeline + group 0 with
  // its `bucketId` param == this bucket + any pass-specific group-2 engine
  // resources) — or nullptr to skip the bucket. Draw() then binds group 1
  // (compacted + bucketBase) on it, sets the bucket's mesh, and issues the
  // bucket's DrawIndexedIndirect.
  using BucketMaterialFn =
      std::function<RenderingMaterialInstance*(uint32_t bucket)>;

  // Issues one DrawIndexedIndirect per configured bucket, indexing the
  // GPU-computed per-bucket offsets via the vertex shader (see the class
  // comment). Buckets with a GPU survivor count of 0 draw nothing automatically
  // (indirect instanceCount == 0) — no CPU readback of the counts is needed.
  // Called inside an active render pass, after Cull() on the same encoder.
  void Draw(RenderPassContext& pass, FrameContext& frame,
            const BucketMaterialFn& material_for_bucket) const;

  // Test/debug readback accessors (all created with CopySrc). The compacted
  // transforms, the per-bucket counts, the per-bucket bases (prefix-sum output),
  // and the per-bucket indirect args. Plus the byte stride of one indirect-args
  // struct and the byte offset of its instanceCount field.
  wgpu::Buffer GetCompactedBuffer() const { return compacted_buffer_; }
  wgpu::Buffer GetBucketCountBuffer() const { return bucket_count_buffer_; }
  wgpu::Buffer GetBucketBaseBuffer() const { return bucket_base_buffer_; }
  wgpu::Buffer GetArgsBuffer() const { return args_buffer_; }
  uint32_t GetCompactedCapacity() const { return compacted_capacity_; }
  static constexpr uint64_t kArgsStride = 20;
  static constexpr uint64_t kArgsInstanceCountOffset = 4;

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  uint32_t capacity_ = 0;
  uint32_t num_models_ = 0;
  uint32_t num_buckets_ = 0;
  uint32_t compacted_capacity_ = 0;  // slots in compacted_buffer_ (== capacity_; tight packing, no padding)
  uint32_t instance_count_ = 0;
  std::array<float, kMaxLods - 1> lod_thresholds_{};

  std::shared_ptr<const CompiledComputePipeline> classify_pipeline_;
  std::shared_ptr<const CompiledComputePipeline> scan_pipeline_;
  std::shared_ptr<const CompiledComputePipeline> scatter_pipeline_;

  wgpu::Buffer config_buffer_;               // CullConfig uniform (144 bytes)
  wgpu::Buffer instance_buffer_;             // InstanceData[capacity]
  wgpu::Buffer per_instance_bucket_buffer_;  // u32[capacity] (SENTINEL = culled)
  wgpu::Buffer bucket_count_buffer_;         // atomic<u32>[numBuckets]
  wgpu::Buffer bucket_base_buffer_;          // u32[numBuckets] (scan output)
  wgpu::Buffer write_cursor_buffer_;         // atomic<u32>[numBuckets]
  wgpu::Buffer compacted_buffer_;            // mat4x4<f32>[compacted_capacity_]
  wgpu::Buffer args_buffer_;                 // IndirectArgs[numBuckets]

  wgpu::BindGroup classify_bind_group_;
  wgpu::BindGroup scan_bind_group_;
  wgpu::BindGroup scatter_bind_group_;

  struct BucketMesh {
    wgpu::Buffer vertex_buffer;
    wgpu::Buffer index_buffer;
    wgpu::IndexFormat index_format = wgpu::IndexFormat::Uint32;
    uint32_t index_count = 0;
  };
  std::vector<BucketMesh> bucket_meshes_;  // size num_buckets_
};

}  // namespace badlands
