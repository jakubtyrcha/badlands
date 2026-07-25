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

// Mirrors `FrustumUniform` in shaders/compute/instance_cull.wesl
// byte-for-byte (128 bytes: 6 planes + camera pos + counts, all 16-byte
// aligned with no padding gaps).
struct FrustumUniformData {
  std::array<glm::vec4, 6> planes{};
  glm::vec4 camera_world_pos{0.0f};
  glm::uvec4 counts{0u};  // x = instance count, yzw unused
};
static_assert(sizeof(FrustumUniformData) == 128);

// Mirrors `IndirectArgs` in shaders/compute/instance_cull.wesl (and the
// standard DrawIndexedIndirect args layout from Phase A) byte-for-byte.
struct IndirectArgsData {
  uint32_t index_count = 0;
  uint32_t instance_count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  uint32_t first_instance = 0;
};
static_assert(sizeof(IndirectArgsData) == 20);

}  // namespace

GpuInstanceRenderer::GpuInstanceRenderer(wgpu::Device device, wgpu::Queue queue,
                                         GpuPipelineGenerator& pipeline_generator,
                                         uint32_t capacity)
    : device_(device), queue_(queue), capacity_(capacity) {
  cull_pipeline_ = pipeline_generator.GetComputePipeline("compute/instance_cull");
  if (!cull_pipeline_) {
    spdlog::error(
        "GpuInstanceRenderer: failed to compile compute/instance_cull");
    return;
  }
  if (cull_pipeline_->bind_group_layouts.size() < 2) {
    spdlog::error(
        "GpuInstanceRenderer: compute/instance_cull reflected {} bind "
        "groups, expected 2",
        cull_pipeline_->bind_group_layouts.size());
    cull_pipeline_ = nullptr;
    return;
  }

  const uint64_t instance_buffer_size =
      std::max<uint64_t>(uint64_t{capacity_} * sizeof(InstanceInput), 1);
  wgpu::BufferDescriptor instance_bd{};
  instance_bd.size = instance_buffer_size;
  instance_bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  instance_buffer_ = device_.CreateBuffer(&instance_bd);

  const uint64_t compacted_buffer_size =
      std::max<uint64_t>(uint64_t{capacity_} * sizeof(glm::mat4), 1);
  wgpu::BufferDescriptor compacted_bd{};
  compacted_bd.size = compacted_buffer_size;
  compacted_bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                       wgpu::BufferUsage::CopySrc;
  compacted_buffer_ = device_.CreateBuffer(&compacted_bd);

  wgpu::BufferDescriptor args_bd{};
  args_bd.size = sizeof(IndirectArgsData);
  args_bd.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::Storage |
                 wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
  args_buffer_ = device_.CreateBuffer(&args_bd);
  IndirectArgsData zero_args{};
  queue_.WriteBuffer(args_buffer_, 0, &zero_args, sizeof(zero_args));

  wgpu::BufferDescriptor frustum_bd{};
  frustum_bd.size = sizeof(FrustumUniformData);
  frustum_bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  frustum_uniform_buffer_ = device_.CreateBuffer(&frustum_bd);
  FrustumUniformData zero_frustum{};
  queue_.WriteBuffer(frustum_uniform_buffer_, 0, &zero_frustum,
                     sizeof(zero_frustum));

  // Bind groups reference stable buffer OBJECTS (their contents are
  // overwritten in place via WriteBuffer/atomics), so both are built once
  // here rather than per Cull() call.
  std::array<wgpu::BindGroupEntry, 1> group0_entries{};
  group0_entries[0].binding = 0;
  group0_entries[0].buffer = frustum_uniform_buffer_;
  group0_entries[0].size = sizeof(FrustumUniformData);
  wgpu::BindGroupDescriptor bg0_desc{};
  bg0_desc.layout = cull_pipeline_->bind_group_layouts[0];
  bg0_desc.entryCount = group0_entries.size();
  bg0_desc.entries = group0_entries.data();
  bind_group_0_ = device_.CreateBindGroup(&bg0_desc);

  std::array<wgpu::BindGroupEntry, 3> group1_entries{};
  group1_entries[0].binding = 0;
  group1_entries[0].buffer = instance_buffer_;
  group1_entries[0].size = instance_buffer_size;
  group1_entries[1].binding = 1;
  group1_entries[1].buffer = compacted_buffer_;
  group1_entries[1].size = compacted_buffer_size;
  group1_entries[2].binding = 2;
  group1_entries[2].buffer = args_buffer_;
  group1_entries[2].size = sizeof(IndirectArgsData);
  wgpu::BindGroupDescriptor bg1_desc{};
  bg1_desc.layout = cull_pipeline_->bind_group_layouts[1];
  bg1_desc.entryCount = group1_entries.size();
  bg1_desc.entries = group1_entries.data();
  bind_group_1_ = device_.CreateBindGroup(&bg1_desc);
}

void GpuInstanceRenderer::UploadInstances(
    std::span<const InstanceInput> instances) {
  if (!IsValid()) {
    return;
  }
  uint32_t count = static_cast<uint32_t>(instances.size());
  if (count > capacity_) {
    spdlog::error(
        "GpuInstanceRenderer::UploadInstances: {} instances exceeds "
        "capacity {} — truncating",
        instances.size(), capacity_);
    count = capacity_;
  }
  instance_count_ = count;
  if (count > 0) {
    queue_.WriteBuffer(instance_buffer_, 0, instances.data(),
                       uint64_t{count} * sizeof(InstanceInput));
  }
}

void GpuInstanceRenderer::SetMesh(wgpu::Buffer vertex_buffer,
                                 wgpu::Buffer index_buffer,
                                 wgpu::IndexFormat index_format,
                                 uint32_t index_count) {
  vertex_buffer_ = vertex_buffer;
  index_buffer_ = index_buffer;
  index_format_ = index_format;
  index_count_ = index_count;

  if (!IsValid()) {
    return;
  }
  // Only the mesh-derived fields change here; instanceCount is left as-is
  // (Cull() clears + repopulates it every dispatch).
  IndirectArgsData args{};
  args.index_count = index_count_;
  args.instance_count = 0;
  args.first_index = 0;
  args.base_vertex = 0;
  args.first_instance = 0;
  queue_.WriteBuffer(args_buffer_, 0, &args, sizeof(args));
}

void GpuInstanceRenderer::Cull(FrameContext& frame, const Camera& camera) {
  if (!IsValid()) {
    return;
  }

  const Frustum frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());
  FrustumUniformData uniform_data{};
  for (int i = 0; i < 6; ++i) {
    uniform_data.planes[static_cast<size_t>(i)] = frustum.planes[i];
  }
  uniform_data.camera_world_pos = glm::vec4(camera.GetPosition(), 0.0f);
  uniform_data.counts = glm::uvec4(instance_count_, 0u, 0u, 0u);
  queue_.WriteBuffer(frustum_uniform_buffer_, 0, &uniform_data,
                     sizeof(uniform_data));

  frame.GetEncoder().ClearBuffer(args_buffer_, kArgsInstanceCountOffset, 4);

  const uint32_t workgroup_size = std::max(1u, cull_pipeline_->workgroup_size[0]);
  const uint32_t dispatch_count =
      (instance_count_ + workgroup_size - 1) / workgroup_size;

  wgpu::ComputePassEncoder compute_pass = frame.BeginComputePass();
  compute_pass.SetPipeline(cull_pipeline_->pipeline);
  compute_pass.SetBindGroup(0, bind_group_0_, 0, nullptr);
  compute_pass.SetBindGroup(1, bind_group_1_, 0, nullptr);
  if (dispatch_count > 0) {
    compute_pass.DispatchWorkgroups(dispatch_count, 1, 1);
  }
  compute_pass.End();
}

bool GpuInstanceRenderer::Draw(RenderPassContext& pass, FrameContext& frame,
                               RenderingMaterialInstance& instance) const {
  if (!IsValid() || !vertex_buffer_ || !index_buffer_) {
    return false;
  }
  if (!instance.BindInstanceData(pass, frame, compacted_buffer_, 0)) {
    return false;
  }
  pass.SetVertexBuffer(0, vertex_buffer_);
  pass.SetIndexBuffer(index_buffer_, index_format_);
  pass.DrawIndexedIndirect(args_buffer_, 0);
  return true;
}

}  // namespace badlands
