#pragma once

// Ported from sampo's src/rendering/context/render_pass_context.{hpp,cpp},
// namespace sampo -> badlands.
//
// Deviation: `SetMaterial(const MeshRenderingMaterial&)` is dropped — that
// overload belongs to sampo's material system (rendering/material/
// material.hpp), which hasn't been ported to badlands yet (see the
// deviation note in gpu_pipeline_generator.hpp). `SetPipeline` (taking a raw
// wgpu::RenderPipeline, e.g. from CompiledPipeline::pipeline) covers what
// this port's callers need.
#include <dawn/webgpu_cpp.h>

namespace badlands {

// RenderPassContext wraps wgpu::RenderPassEncoder with a small convenience
// API mirroring the raw encoder's draw/bind-group/pipeline calls.
class RenderPassContext {
 public:
  RenderPassContext() = default;
  explicit RenderPassContext(wgpu::RenderPassEncoder encoder);

  // Non-copyable
  RenderPassContext(const RenderPassContext&) = delete;
  RenderPassContext& operator=(const RenderPassContext&) = delete;

  // Movable
  RenderPassContext(RenderPassContext&& other) noexcept;
  RenderPassContext& operator=(RenderPassContext&& other) noexcept;

  // Pipeline
  void SetPipeline(wgpu::RenderPipeline pipeline);

  // Bind groups
  void SetBindGroup(uint32_t group, wgpu::BindGroup bg);
  void SetBindGroup(uint32_t group, wgpu::BindGroup bg,
                    uint32_t dynamic_offset_count,
                    const uint32_t* dynamic_offsets);

  // Vertex buffers
  void SetVertexBuffer(uint32_t slot, wgpu::Buffer buffer, uint64_t offset = 0,
                       uint64_t size = WGPU_WHOLE_SIZE);

  // Index buffer (for indexed drawing)
  void SetIndexBuffer(wgpu::Buffer buffer, wgpu::IndexFormat format,
                      uint64_t offset = 0, uint64_t size = WGPU_WHOLE_SIZE);

  // Drawing
  void Draw(uint32_t vertex_count, uint32_t instance_count = 1,
            uint32_t first_vertex = 0, uint32_t first_instance = 0);
  void DrawIndexed(uint32_t index_count, uint32_t instance_count = 1,
                   uint32_t first_index = 0, int32_t base_vertex = 0,
                   uint32_t first_instance = 0);

  // End the render pass
  void End();

  // Check if pass is valid
  bool IsValid() const { return encoder_ != nullptr; }

  // Access underlying encoder (for ImGui, etc.)
  wgpu::RenderPassEncoder GetEncoder() const { return encoder_; }

 private:
  wgpu::RenderPassEncoder encoder_;
};

}  // namespace badlands
