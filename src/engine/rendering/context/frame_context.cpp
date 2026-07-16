// Ported from sampo's src/rendering/context/frame_context.cpp, namespace
// sampo -> badlands. The Material-based CreateBindGroup overload is dropped
// — see the header's deviation note.
#include "engine/rendering/context/frame_context.hpp"

#include <cstring>

namespace badlands {

FrameContext::~FrameContext() {
  if (active_) {
    ReleaseTransientResources();
  }
}

FrameContext::FrameContext(FrameContext&& other) noexcept
    : device_(other.device_),
      queue_(other.queue_),
      encoder_(other.encoder_),
      frame_uniform_buffer_(other.frame_uniform_buffer_),
      transient_buffers_(std::move(other.transient_buffers_)),
      transient_bind_groups_(std::move(other.transient_bind_groups_)),
      dynamic_uniform_buffer_(other.dynamic_uniform_buffer_),
      dynamic_buffer_size_(other.dynamic_buffer_size_),
      dynamic_offset_(other.dynamic_offset_),
      uniform_alignment_(other.uniform_alignment_),
      defer_buffer_callback_(std::move(other.defer_buffer_callback_)),
      active_(other.active_) {
  other.device_ = nullptr;
  other.queue_ = nullptr;
  other.encoder_ = nullptr;
  other.frame_uniform_buffer_ = nullptr;
  other.dynamic_uniform_buffer_ = nullptr;
  other.dynamic_buffer_size_ = 0;
  other.dynamic_offset_ = 0;
  other.active_ = false;
}

FrameContext& FrameContext::operator=(FrameContext&& other) noexcept {
  if (this != &other) {
    if (active_) {
      ReleaseTransientResources();
    }
    device_ = other.device_;
    queue_ = other.queue_;
    encoder_ = other.encoder_;
    frame_uniform_buffer_ = other.frame_uniform_buffer_;
    transient_buffers_ = std::move(other.transient_buffers_);
    transient_bind_groups_ = std::move(other.transient_bind_groups_);
    dynamic_uniform_buffer_ = other.dynamic_uniform_buffer_;
    dynamic_buffer_size_ = other.dynamic_buffer_size_;
    dynamic_offset_ = other.dynamic_offset_;
    uniform_alignment_ = other.uniform_alignment_;
    defer_buffer_callback_ = std::move(other.defer_buffer_callback_);
    active_ = other.active_;

    other.device_ = nullptr;
    other.queue_ = nullptr;
    other.encoder_ = nullptr;
    other.frame_uniform_buffer_ = nullptr;
    other.dynamic_uniform_buffer_ = nullptr;
    other.dynamic_buffer_size_ = 0;
    other.dynamic_offset_ = 0;
    other.active_ = false;
  }
  return *this;
}

void FrameContext::Begin(wgpu::Device device, wgpu::Queue queue,
                         const UniformData& frame_uniforms,
                         uint32_t uniform_alignment) {
  device_ = device;
  queue_ = queue;
  uniform_alignment_ = uniform_alignment;
  active_ = true;

  // Reset dynamic buffer offset for new frame
  dynamic_offset_ = 0;

  // Create command encoder
  wgpu::CommandEncoderDescriptor encoder_desc;
  encoder_ = device_.CreateCommandEncoder(&encoder_desc);

  // Create frame uniform buffer
  frame_uniform_buffer_ =
      CreateUniformBuffer(sizeof(UniformData), &frame_uniforms);
}

wgpu::CommandBuffer FrameContext::End() {
  if (!active_) {
    return nullptr;
  }

  // Finish command encoder
  wgpu::CommandBufferDescriptor cmd_desc;
  wgpu::CommandBuffer commands = encoder_.Finish(&cmd_desc);

  // Release transient resources
  ReleaseTransientResources();

  active_ = false;
  encoder_ = nullptr;

  return commands;
}

wgpu::Buffer FrameContext::CreateUniformBuffer(size_t size, const void* data) {
  wgpu::BufferDescriptor desc;
  desc.size = size;
  desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  desc.mappedAtCreation = (data != nullptr);
  desc.label =
      WGPUStringView{.data = "FrameContext_UniformBuffer", .length = 26};

  wgpu::Buffer buffer = device_.CreateBuffer(&desc);

  if (data != nullptr) {
    void* mapped = buffer.GetMappedRange(0, size);
    std::memcpy(mapped, data, size);
    buffer.Unmap();
  }

  transient_buffers_.push_back(buffer);
  return buffer;
}

void FrameContext::ReserveDynamicUniform(size_t total_size) {
  // Grow buffer if needed to accommodate total_size
  if (!dynamic_uniform_buffer_ || total_size > dynamic_buffer_size_) {
    constexpr size_t kInitialSize = 256 * 1024;
    size_t new_size =
        dynamic_buffer_size_ == 0 ? kInitialSize : dynamic_buffer_size_;
    while (new_size < total_size) {
      new_size *= 2;
    }

    wgpu::BufferDescriptor desc;
    desc.size = new_size;
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    desc.label =
        WGPUStringView{.data = "FrameContext_DynamicUniform", .length = 27};

    dynamic_uniform_buffer_ = device_.CreateBuffer(&desc);
    dynamic_buffer_size_ = new_size;
  }
}

uint32_t FrameContext::AllocateDynamicUniform(size_t size, const void* data) {
  // Calculate aligned size
  size_t aligned_size = (size + uniform_alignment_ - 1) &
                        ~(static_cast<size_t>(uniform_alignment_) - 1);

  // Grow buffer if needed (start with 256KB, grow as needed)
  constexpr size_t kInitialSize = 256 * 1024;
  if (!dynamic_uniform_buffer_ ||
      dynamic_offset_ + aligned_size > dynamic_buffer_size_) {
    size_t new_size =
        dynamic_buffer_size_ == 0 ? kInitialSize : dynamic_buffer_size_ * 2;
    while (new_size < dynamic_offset_ + aligned_size) {
      new_size *= 2;
    }

    wgpu::BufferDescriptor desc;
    desc.size = new_size;
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    desc.label =
        WGPUStringView{.data = "FrameContext_DynamicUniform", .length = 27};

    dynamic_uniform_buffer_ = device_.CreateBuffer(&desc);
    dynamic_buffer_size_ = new_size;
  }

  uint32_t offset = static_cast<uint32_t>(dynamic_offset_);

  // Write data to buffer at current offset
  if (data != nullptr) {
    queue_.WriteBuffer(dynamic_uniform_buffer_, offset, data, size);
  }

  dynamic_offset_ += aligned_size;
  return offset;
}

RenderPassContext FrameContext::BeginRenderPass(
    const wgpu::RenderPassDescriptor& desc) {
  wgpu::RenderPassEncoder encoder = encoder_.BeginRenderPass(&desc);
  return RenderPassContext(encoder);
}

wgpu::ComputePassEncoder FrameContext::BeginComputePass(
    const wgpu::PassTimestampWrites* timestamp_writes) {
  wgpu::ComputePassDescriptor desc;
  desc.timestampWrites = timestamp_writes;
  return encoder_.BeginComputePass(&desc);
}

wgpu::BindGroup FrameContext::CreateBindGroup(
    wgpu::BindGroupLayout layout,
    std::span<const wgpu::BindGroupEntry> entries) {
  wgpu::BindGroupDescriptor desc;
  desc.layout = layout;
  desc.entryCount = entries.size();
  desc.entries = entries.data();

  wgpu::BindGroup bg = device_.CreateBindGroup(&desc);
  if (bg) {
    transient_bind_groups_.push_back(bg);
  }
  return bg;
}

void FrameContext::ReleaseTransientResources() {
  // WebGPU C++ wrappers handle release via destructor
  // Just clear the vectors
  transient_buffers_.clear();
  transient_bind_groups_.clear();
  frame_uniform_buffer_ = nullptr;
}

void FrameContext::DeferBuffer(wgpu::Buffer buffer) {
  if (buffer && defer_buffer_callback_) {
    defer_buffer_callback_(std::move(buffer));
  }
  // If no callback set, buffer will be destroyed immediately by RAII
  // This is the fallback behavior if the Renderer hasn't set a callback
}

}  // namespace badlands
