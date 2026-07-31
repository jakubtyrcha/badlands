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
// Submesh dimension: a bucket's ONE compacted transform slice can drive
// SEVERAL draws — e.g. distinct geometry/materials in different render
// passes — by adding a `submesh` index orthogonal to `bucket`. Indirect-args
// and mesh bindings are addressed per SLOT (`bucket*GetNumSubmeshes()+submesh`);
// the scan still computes exactly one survivor count per bucket and publishes
// it into every one of that bucket's submesh slots, so all submeshes of a
// bucket draw the SAME survivors. `num_submeshes=1` (the default) is the
// original single-draw-per-bucket behavior.
//
// The caller resolves and binds whatever RenderingMaterialInstance it wants per
// (bucket, submesh) (e.g. via MaterialInstanceCache — see the tests) and hands
// them to `Draw()` through a callback; this class never reaches into a
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
struct Frustum;

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
  // dist < [1], … coarsest otherwise). `num_submeshes` (clamped >= 1) is the
  // number of draw slots per bucket — see the class comment's submesh section;
  // 1 (the default) is a single draw per bucket. Compiles the three compute
  // pipelines immediately; IsValid() reports whether that succeeded.
  GpuInstanceRenderer(wgpu::Device device, wgpu::Queue queue,
                      GpuPipelineGenerator& pipeline_generator,
                      uint32_t capacity, uint32_t num_models,
                      std::array<float, kMaxLods - 1> lod_thresholds,
                      uint32_t num_submeshes = 1);

  bool IsValid() const {
    return classify_pipeline_ != nullptr && scan_pipeline_ != nullptr &&
           scatter_pipeline_ != nullptr;
  }

  // Which cull/compaction result Draw() reads from. kMain is the
  // camera-frustum result from Cull(); kShadow is the light-frustum result
  // from CullShadow() -- a SEPARATE buffer set (see the class comment on
  // CullShadow for why one config buffer cannot serve both in the same
  // frame). A never-culled shadow set (CullShadow() never called this frame)
  // draws nothing: its args buffers are zero-initialized at construction and
  // only ever written by a scan pass, so instanceCount stays 0.
  enum class CullSet { kMain, kShadow };

  uint32_t GetCapacity() const { return capacity_; }
  uint32_t GetNumModels() const { return num_models_; }
  uint32_t GetNumBuckets() const { return num_buckets_; }
  uint32_t GetNumSubmeshes() const { return num_submeshes_; }
  // The bucket id a given (model, lod) routes to — the CPU-known per-draw
  // constant the vertex shader uses to look up its base offset.
  static uint32_t BucketId(uint32_t model_id, uint32_t lod) {
    return model_id * kMaxLods + lod;
  }

  // Replaces the static instance-input set wholesale. `instances.size()` must be
  // <= capacity (larger inputs are truncated, logged as an error).
  void UploadInstances(std::span<const InstanceInput> instances);

  // Configure one (bucket, submesh) slot's geometry (the mesh drawn per
  // surviving instance of `bucket` for this submesh) and pre-fill its
  // indirect-args indexCount (firstIndex/baseVertex stay 0 -- each slot binds
  // its own index buffer, so no shared-buffer offset is needed). The vertex
  // layout must match whatever instanced material `Draw()` later resolves for
  // the slot. Slot = bucket*GetNumSubmeshes()+submesh; slots left unconfigured
  // (index_count 0) are skipped by Draw(). `bucket` must be < GetNumBuckets(),
  // `submesh` must be < GetNumSubmeshes().
  void SetBucketSubmesh(uint32_t bucket, uint32_t submesh,
                        wgpu::Buffer vertex_buffer, wgpu::Buffer index_buffer,
                        wgpu::IndexFormat index_format, uint32_t index_count);

  // Clears the per-bucket counts, derives the world-space frustum + camera
  // position from `camera`, uploads the cull config (frustum + thresholds +
  // counts), and dispatches the three compute passes (classify+count → scan →
  // scatter). See the class comment for the encoder-sequencing requirement
  // (must precede BeginRenderPass).
  void Cull(FrameContext& frame, const Camera& camera);

  // Same 3-pass cull/LOD/compaction pipeline as Cull(), but against the
  // LIGHT's frustum (`Frustum::FromViewProj(light_view_proj)`) instead of the
  // camera's, writing into the DEDICATED shadow buffer set (Draw(...,
  // CullSet::kShadow) reads it) rather than the main one. `camera` still
  // supplies camera_world_pos and the LOD-selection thresholds: shadow
  // geometry must match the LOD the camera would actually render, not a
  // light-distance LOD. A SEPARATE buffer set exists (not just a second call
  // to Cull() with different config contents) because queue_.WriteBuffer
  // uploads execute BEFORE the frame's command buffer runs -- two Cull()-like
  // calls sharing one config buffer in the same frame would race, with
  // whichever WriteBuffer lands last silently winning for BOTH dispatches.
  // Same encoder-sequencing requirement as Cull(): before any render pass
  // opens on this frame's encoder. LAZY: the shadow buffer set/bind groups
  // are only allocated on first need (see EnsureShadowCull()) -- this call
  // provisions them if nothing already has.
  void CullShadow(FrameContext& frame, const Camera& camera,
                  const glm::mat4& light_view_proj);

  // Provisions the shadow cull buffer set + bind groups (see the class
  // comment's "Second buffer set" section) if they don't exist yet, WITHOUT
  // dispatching a cull -- a no-op if CullShadow() (or an earlier
  // EnsureShadowCull()) already created them. A field with a shadow-casting
  // submesh calls this when the submesh is registered (see
  // InstancedMeshField::SetSubmeshShadow) so the shadow-set accessors and a
  // subsequent SetBucketSubmesh's shadow-args mirroring both see live buffers
  // ahead of the first real CullShadow() dispatch. Idempotent; safe to call
  // even if this renderer never ends up casting a shadow (nothing else reads
  // the resources it provisions until Draw(..., CullSet::kShadow) or
  // CullShadow() do).
  void EnsureShadowCull();

  // Per-(bucket,submesh) material resolver: given a bucket id and submesh
  // index, return the material instance to render that slot with — already
  // bound (pipeline + group 0 with its `bucketId` param == this bucket + any
  // pass-specific group-2 engine resources) — or nullptr to skip the slot.
  // Draw() then binds group 1 (compacted + bucketBase) on it, sets the slot's
  // mesh, and issues the slot's DrawIndexedIndirect. All submeshes of a bucket
  // share the SAME bucketBase entry, so they draw the same compacted slice.
  using BucketSubmeshMaterialFn = std::function<RenderingMaterialInstance*(
      uint32_t bucket, uint32_t submesh)>;

  // Issues one DrawIndexedIndirect per configured (bucket, submesh) slot,
  // indexing the GPU-computed per-bucket offsets via the vertex shader (see
  // the class comment). Slots with a GPU survivor count of 0 draw nothing
  // automatically (indirect instanceCount == 0) — no CPU readback of the
  // counts is needed. Called inside an active render pass, after Cull()
  // (`cull_set` == kMain, the default) or CullShadow() (`cull_set` ==
  // kShadow) ran earlier on the same encoder. `cull_set` == kShadow is
  // ADDITIONALLY safe to call before the shadow buffer set has ever been
  // provisioned (no CullShadow()/EnsureShadowCull() call yet this renderer's
  // lifetime) — a plain no-op, not a null-buffer validation error, since a
  // field with no shadow-casting submesh never triggers either of those (see
  // scene_renderer.cpp's unconditional per-field Draw(kShadow) call).
  void Draw(RenderPassContext& pass, FrameContext& frame,
            const BucketSubmeshMaterialFn& material_for_bucket_submesh,
            CullSet cull_set = CullSet::kMain) const;

  // Test/debug readback accessors (all created with CopySrc). The compacted
  // transforms, the per-bucket counts, the per-bucket bases (prefix-sum output),
  // and the per-bucket indirect args. Plus the byte stride of one indirect-args
  // struct and the byte offset of its instanceCount field.
  wgpu::Buffer GetCompactedBuffer() const { return compacted_buffer_; }
  wgpu::Buffer GetBucketCountBuffer() const { return bucket_count_buffer_; }
  wgpu::Buffer GetBucketBaseBuffer() const { return bucket_base_buffer_; }
  wgpu::Buffer GetArgsBuffer() const { return args_buffer_; }
  // Same, for the shadow cull set (CullShadow()'s output) -- see CullShadow's
  // doc comment for why this is a distinct buffer set, not a re-read of the
  // above after a second Cull()-like call. LAZY: return a null wgpu::Buffer
  // until the shadow set has actually been provisioned (CullShadow() or
  // EnsureShadowCull() ran at least once) -- see those methods' doc comments.
  wgpu::Buffer GetShadowCompactedBuffer() const { return compacted_buffer_shadow_; }
  wgpu::Buffer GetShadowBucketCountBuffer() const { return bucket_count_buffer_shadow_; }
  wgpu::Buffer GetShadowBucketBaseBuffer() const { return bucket_base_buffer_shadow_; }
  wgpu::Buffer GetShadowArgsBuffer() const { return args_buffer_shadow_; }
  uint32_t GetCompactedCapacity() const { return compacted_capacity_; }
  static constexpr uint64_t kArgsStride = 20;
  static constexpr uint64_t kArgsInstanceCountOffset = 4;

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  uint32_t capacity_ = 0;
  uint32_t num_models_ = 0;
  uint32_t num_submeshes_ = 1;
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
  wgpu::Buffer args_buffer_;  // IndirectArgs[numBuckets*numSubmeshes], slot = bucket*numSubmeshes+submesh

  wgpu::BindGroup classify_bind_group_;
  wgpu::BindGroup scan_bind_group_;
  wgpu::BindGroup scatter_bind_group_;

  // Second buffer set + bind-group trio, dedicated to CullShadow()'s
  // light-frustum cull -- mirrors the main set above field-for-field (same
  // sizes, same usage flags, including CopySrc on compacted_buffer_shadow_/
  // args_buffer_shadow_ so tests can read them back the same way as the main
  // set's). instance_buffer_ is SHARED (both culls classify the same static
  // per-instance input); everything downstream of classify is per-set so the
  // two culls' outputs never alias. See CullShadow's doc comment for why a
  // second config buffer is required rather than reusing config_buffer_.
  //
  // LAZY: unlike the main set above, these are NOT allocated by the
  // constructor -- most fields never cast a shadow, and a shadow-casting-
  // capable renderer that's constructed well before its first real cull
  // shouldn't pay for 7 extra buffers + 3 bind groups it may never use. See
  // EnsureShadowCullResources().
  wgpu::Buffer config_buffer_shadow_;
  wgpu::Buffer per_instance_bucket_buffer_shadow_;
  wgpu::Buffer bucket_count_buffer_shadow_;
  wgpu::Buffer bucket_base_buffer_shadow_;
  wgpu::Buffer write_cursor_buffer_shadow_;
  wgpu::Buffer compacted_buffer_shadow_;
  wgpu::Buffer args_buffer_shadow_;

  wgpu::BindGroup classify_bind_group_shadow_;
  wgpu::BindGroup scan_bind_group_shadow_;
  wgpu::BindGroup scatter_bind_group_shadow_;

  // True once EnsureShadowCullResources() has allocated the shadow buffer
  // set + bind groups above (idempotent thereafter). Gates: the shadow-args
  // mirroring half of SetBucketSubmesh (skipped pre-creation -- the eventual
  // creation REPLAYS every configured slot's geometry instead, see
  // EnsureShadowCullResources()), and Draw(..., CullSet::kShadow)'s no-op
  // guard (the shadow buffers are null pre-creation; issuing a
  // DrawIndexedIndirect against a null args buffer would be a Dawn validation
  // error, not the safe no-op Draw()'s doc comment promises).
  bool shadow_resources_created_ = false;

  // Shared body of Cull()/CullShadow(): uploads `config` (the frustum planes
  // + camera_world_pos + LOD thresholds + counts), clears `bucket_count`, and
  // dispatches classify/scan/scatter against the given bind-group trio. See
  // Cull()'s .cpp comment for why the per-pass barriers need no manual sync.
  void CullInternal(FrameContext& frame, const Frustum& frustum,
                    glm::vec3 camera_world_pos, wgpu::Buffer config_buffer,
                    wgpu::Buffer bucket_count_buffer,
                    wgpu::BindGroup classify_bind_group,
                    wgpu::BindGroup scan_bind_group,
                    wgpu::BindGroup scatter_bind_group);

  // Allocates the shadow buffer set + bind groups (config_buffer_shadow_
  // through scatter_bind_group_shadow_) if shadow_resources_created_ is still
  // false; a no-op otherwise. On creation: same buffer sizes/usages the
  // constructor used to build these with eagerly, zero-initializes
  // args_buffer_shadow_, builds the 3 bind groups, then REPLAYS every
  // configured (bucket,submesh) slot's indirect-args GEOMETRY fields
  // (indexCount@0, firstIndex/baseVertex/firstInstance@8 -- see
  // SetBucketSubmesh's .cpp comment for the field layout) from
  // submesh_meshes_ into args_buffer_shadow_, so a slot configured via
  // SetBucketSubmesh BEFORE this first creation still has its geometry
  // prefilled (SetBucketSubmesh itself only mirrors into the shadow args
  // buffer when shadow_resources_created_ is already true -- see its .cpp
  // comment). Called by CullShadow() and by the public EnsureShadowCull().
  void EnsureShadowCullResources();

  struct SubmeshMesh {
    wgpu::Buffer vertex_buffer;
    wgpu::Buffer index_buffer;
    wgpu::IndexFormat index_format = wgpu::IndexFormat::Uint32;
    uint32_t index_count = 0;
  };
  std::vector<SubmeshMesh> submesh_meshes_;  // size num_buckets_ * num_submeshes_
};

}  // namespace badlands
