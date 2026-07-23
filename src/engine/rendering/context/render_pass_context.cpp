// Ported from sampo's src/rendering/context/render_pass_context.cpp,
// namespace sampo -> badlands. SetMaterial dropped — see the header's
// deviation note.
#include "engine/rendering/context/render_pass_context.hpp"

namespace badlands {

RenderPassContext::RenderPassContext(wgpu::RenderPassEncoder encoder)
    : encoder_(encoder) {}

RenderPassContext::RenderPassContext(RenderPassContext&& other) noexcept
    : encoder_(other.encoder_) {
  other.encoder_ = nullptr;
}

RenderPassContext& RenderPassContext::operator=(
    RenderPassContext&& other) noexcept {
  if (this != &other) {
    encoder_ = other.encoder_;
    other.encoder_ = nullptr;
  }
  return *this;
}

void RenderPassContext::SetPipeline(wgpu::RenderPipeline pipeline) {
  if (encoder_ && pipeline) {
    encoder_.SetPipeline(pipeline);
  }
}

void RenderPassContext::SetBindGroup(uint32_t group, wgpu::BindGroup bg) {
  if (encoder_ && bg) {
    encoder_.SetBindGroup(group, bg, 0, nullptr);
  }
}

void RenderPassContext::SetBindGroup(uint32_t group, wgpu::BindGroup bg,
                                     uint32_t dynamic_offset_count,
                                     const uint32_t* dynamic_offsets) {
  if (encoder_ && bg) {
    encoder_.SetBindGroup(group, bg, dynamic_offset_count, dynamic_offsets);
  }
}

void RenderPassContext::SetVertexBuffer(uint32_t slot, wgpu::Buffer buffer,
                                        uint64_t offset, uint64_t size) {
  if (encoder_ && buffer) {
    encoder_.SetVertexBuffer(slot, buffer, offset, size);
  }
}

void RenderPassContext::SetIndexBuffer(wgpu::Buffer buffer,
                                       wgpu::IndexFormat format,
                                       uint64_t offset, uint64_t size) {
  if (encoder_ && buffer) {
    encoder_.SetIndexBuffer(buffer, format, offset, size);
  }
}

void RenderPassContext::Draw(uint32_t vertex_count, uint32_t instance_count,
                             uint32_t first_vertex, uint32_t first_instance) {
  if (encoder_) {
    encoder_.Draw(vertex_count, instance_count, first_vertex, first_instance);
  }
}

void RenderPassContext::DrawIndexed(uint32_t index_count,
                                    uint32_t instance_count,
                                    uint32_t first_index, int32_t base_vertex,
                                    uint32_t first_instance) {
  if (encoder_) {
    encoder_.DrawIndexed(index_count, instance_count, first_index, base_vertex,
                         first_instance);
  }
}

void RenderPassContext::SetScissorRect(uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height) {
  if (encoder_) {
    encoder_.SetScissorRect(x, y, width, height);
  }
}

void RenderPassContext::End() {
  if (encoder_) {
    encoder_.End();
    encoder_ = nullptr;
  }
}

}  // namespace badlands
