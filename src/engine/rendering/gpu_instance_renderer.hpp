#pragma once

// GPU-driven instanced rendering, Phase C: a compute frustum-cull +
// compaction pass feeding one indirect instanced draw. Engine-generic — no
// scene/ECS/material-cache assumptions. Owns the instance-input, compacted,
// and indirect-args buffers plus the cull compute pipeline; the caller
// resolves and `Bind()`s whatever `RenderingMaterialInstance` it wants
// rendered (e.g. via MaterialInstanceCache — see
// src/engine/tests/gpu_instance_tests.cpp for the pattern) and hands it to
// `Draw()`. A later phase drives the instance-input set from an ECS
// component; this class never reaches into one itself.
//
// Sequencing: `Cull()` dispatches a compute pass on `frame`'s encoder and
// MUST run before any render pass is begun on that same encoder — compute
// and render passes cannot be active simultaneously on one WebGPU command
// encoder. `Draw()` runs inside an already-active render pass, after
// `Cull()` ran earlier on the same encoder; the compute→render buffer
// dependency is handled automatically by the encoder (no manual barrier
// needed in WebGPU).
//
// Single input set / single output bucket: no LOD selection and no
// multi-bucket routing (Phase D's job) — see InstanceInput::model_info,
// reserved but unread this phase.
#include <cstdint>
#include <memory>
#include <span>

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
  // Static per-instance input, uploaded once (or replaced wholesale via
  // UploadInstances). Mirrors `InstanceData` in
  // shaders/compute/instance_cull.wesl byte-for-byte (96 bytes, no padding
  // gaps — see the phase-C report for the WGSL struct-layout arithmetic
  // behind this exact field grouping).
  struct InstanceInput {
    glm::mat4 transform{1.0f};        // world transform
    glm::vec4 bounds_sphere{0.0f};    // xyz = world center, w = radius
    glm::uvec4 model_info{0u};        // x = model id (unused until Phase D)
  };

  // `capacity` upper-bounds both the instance-input set and the compacted
  // output buffer (worst case: every instance survives). Compiles the cull
  // compute pipeline immediately; IsValid() reports whether that succeeded.
  GpuInstanceRenderer(wgpu::Device device, wgpu::Queue queue,
                      GpuPipelineGenerator& pipeline_generator,
                      uint32_t capacity);

  bool IsValid() const { return cull_pipeline_ != nullptr; }

  // Replaces the static instance-input set wholesale. `instances.size()`
  // must be <= the capacity given at construction (larger inputs are
  // truncated, logged as an error).
  void UploadInstances(std::span<const InstanceInput> instances);

  // The mesh drawn per surviving instance (vertex layout must match
  // whatever instanced material `Draw()` is later called with).
  void SetMesh(wgpu::Buffer vertex_buffer, wgpu::Buffer index_buffer,
              wgpu::IndexFormat index_format, uint32_t index_count);

  // Clears the survivor count to 0, derives the world-space frustum from
  // `camera` (Frustum::FromViewProj(camera.GetProj() * camera.GetView())) +
  // camera.GetPosition(), uploads both into the frustum uniform, and
  // dispatches the cull+compact compute pass. See the class comment for the
  // encoder-sequencing requirement (must precede BeginRenderPass).
  void Cull(FrameContext& frame, const Camera& camera);

  // Binds the compacted-transform buffer at `instance`'s group-1 instance
  // slot (BindInstanceData at byte offset 0), sets the mesh's vertex/index
  // buffers, and issues ONE DrawIndexedIndirect of the survivors compacted
  // by the most recent Cull(). The caller must already have called
  // `instance.Bind(pass, frame)` (plus any pass-specific group-2 engine
  // bindings) on `pass` beforehand — mirrors the Bind() / BindInstanceData()
  // sequencing any other instanced draw uses. Returns false if `instance`
  // refuses BindInstanceData (e.g. not an instanced-geometry material).
  bool Draw(RenderPassContext& pass, FrameContext& frame,
           RenderingMaterialInstance& instance) const;

  uint32_t GetCapacity() const { return capacity_; }

  // Test/debug readback accessors: the compacted-transform buffer and the
  // indirect-args buffer (both created with CopySrc so a test can
  // BufferReadback them), plus the byte offset of the args struct's
  // instanceCount field (the cull's atomic counter).
  wgpu::Buffer GetCompactedBuffer() const { return compacted_buffer_; }
  wgpu::Buffer GetArgsBuffer() const { return args_buffer_; }
  static constexpr uint64_t kArgsInstanceCountOffset = 4;

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  uint32_t capacity_ = 0;
  uint32_t instance_count_ = 0;

  std::shared_ptr<const CompiledComputePipeline> cull_pipeline_;

  wgpu::Buffer instance_buffer_;          // InstanceData[capacity], read-only in the cull
  wgpu::Buffer compacted_buffer_;         // mat4x4<f32>[capacity], cull output
  wgpu::Buffer args_buffer_;              // IndirectArgs (20 bytes)
  wgpu::Buffer frustum_uniform_buffer_;   // FrustumUniform (128 bytes)

  wgpu::BindGroup bind_group_0_;  // frustum uniform
  wgpu::BindGroup bind_group_1_;  // instances + compacted + args

  wgpu::Buffer vertex_buffer_;
  wgpu::Buffer index_buffer_;
  wgpu::IndexFormat index_format_ = wgpu::IndexFormat::Uint32;
  uint32_t index_count_ = 0;
};

}  // namespace badlands
