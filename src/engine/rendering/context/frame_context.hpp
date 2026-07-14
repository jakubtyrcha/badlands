#pragma once

// Ported from sampo's src/rendering/context/frame_context.{hpp,cpp},
// namespace sampo -> badlands.
//
// Deviations:
// - `#include "core/camera.hpp"` -> `#include "engine/core/camera.hpp"`:
//   badlands ported Camera/UniformData to src/engine/core/camera.hpp (Task
//   C1), not src/core/camera.hpp where sampo has it.
// - `CreateBindGroup(const MeshRenderingMaterial&, ...)` is dropped — that
//   overload belongs to sampo's material system (rendering/material/
//   material.hpp), not yet ported to badlands (see the deviation note in
//   gpu_pipeline_generator.hpp). `CreateBindGroup(wgpu::BindGroupLayout,
//   ...)` (for bind groups built from an explicit reflected layout) covers
//   what this port's callers need.
#include <functional>
#include <span>
#include <vector>
#include <dawn/webgpu_cpp.h>

#include "engine/core/camera.hpp"
#include "engine/rendering/context/render_pass_context.hpp"

namespace badlands {

// RenderTargetConfig specifies where to render.
// Use nullptr for color_target to render to swapchain.
struct RenderTargetConfig {
  uint32_t width{0};
  uint32_t height{0};
  wgpu::TextureView color_target;  // nullptr = use swapchain
  wgpu::TextureView depth_target;  // nullptr = create internal
};

// FrameContext manages per-frame resources with automatic cleanup.
// Resources created through this context are transient and released at End().
class FrameContext {
 public:
  FrameContext() = default;
  ~FrameContext();

  // Non-copyable
  FrameContext(const FrameContext&) = delete;
  FrameContext& operator=(const FrameContext&) = delete;

  // Movable
  FrameContext(FrameContext&& other) noexcept;
  FrameContext& operator=(FrameContext&& other) noexcept;

  // Lifecycle - called by Renderer
  void Begin(wgpu::Device device, wgpu::Queue queue,
             const UniformData& frame_uniforms,
             uint32_t uniform_alignment = 256);
  wgpu::CommandBuffer End();

  // Transient resource allocation (auto-freed at End())
  wgpu::Buffer CreateUniformBuffer(size_t size, const void* data);
  // Create bind group with explicit layout (for compute shaders / pipelines
  // without a Material)
  wgpu::BindGroup CreateBindGroup(
      wgpu::BindGroupLayout layout,
      std::span<const wgpu::BindGroupEntry> entries);

  // Pass management
  RenderPassContext BeginRenderPass(const wgpu::RenderPassDescriptor& desc);
  wgpu::ComputePassEncoder BeginComputePass();

  // Frame data access
  wgpu::Buffer GetFrameUniformBuffer() const { return frame_uniform_buffer_; }
  wgpu::Device GetDevice() const { return device_; }
  wgpu::CommandEncoder& GetEncoder() { return encoder_; }

  // Dynamic uniform buffer for per-object data
  // Reserve ensures buffer is large enough (call BEFORE creating bind groups)
  void ReserveDynamicUniform(size_t total_size);
  // Allocates space in a single buffer, returns aligned offset
  uint32_t AllocateDynamicUniform(size_t size, const void* data);
  wgpu::Buffer GetDynamicUniformBuffer() const {
    return dynamic_uniform_buffer_;
  }

  // Deferred buffer deletion - schedules buffer for deletion after N frames
  // Use this instead of letting buffers destruct immediately when GPU may still
  // be using them
  using DeferBufferCallback = std::function<void(wgpu::Buffer)>;
  void SetDeferBufferCallback(DeferBufferCallback callback) {
    defer_buffer_callback_ = std::move(callback);
  }
  void DeferBuffer(wgpu::Buffer buffer);

 private:
  void ReleaseTransientResources();

  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::CommandEncoder encoder_;
  wgpu::Buffer frame_uniform_buffer_;

  // Transient resource pool - destroyed at frame end
  std::vector<wgpu::Buffer> transient_buffers_;
  std::vector<wgpu::BindGroup> transient_bind_groups_;

  // Dynamic uniform buffer for per-object data
  wgpu::Buffer dynamic_uniform_buffer_;
  size_t dynamic_buffer_size_ = 0;
  size_t dynamic_offset_ = 0;
  uint32_t uniform_alignment_ = 256;

  // Deferred buffer deletion callback (set by Renderer)
  DeferBufferCallback defer_buffer_callback_;

  bool active_ = false;
};

}  // namespace badlands
