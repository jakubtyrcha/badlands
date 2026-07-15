#include "engine/rendering/texture_readback.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace badlands {

namespace {
constexpr size_t kRowPitchAlignment = 256;

bool IsDepthFormat(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::Depth32Float:
    case wgpu::TextureFormat::Depth24Plus:
    case wgpu::TextureFormat::Depth24PlusStencil8:
    case wgpu::TextureFormat::Depth32FloatStencil8:
    case wgpu::TextureFormat::Depth16Unorm:
      return true;
    default:
      return false;
  }
}
}  // namespace

size_t CalculateAlignedRowPitch(uint32_t width, uint32_t bytes_per_pixel) {
  size_t unaligned = static_cast<size_t>(width) * bytes_per_pixel;
  return (unaligned + kRowPitchAlignment - 1) & ~(kRowPitchAlignment - 1);
}

// TextureReadFuture implementation

TextureReadFuture::TextureReadFuture(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

TextureReadFuture::TextureReadFuture(TextureReadFuture&&) noexcept = default;
TextureReadFuture& TextureReadFuture::operator=(TextureReadFuture&&) noexcept =
    default;
TextureReadFuture::~TextureReadFuture() = default;

bool TextureReadFuture::IsReady() const {
  if (!state_) return true;  // Already consumed or invalid
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->mapping_complete;
}

bool TextureReadFuture::WaitAndConsume() {
  if (!state_) return false;

  std::unique_lock<std::mutex> lock(state_->mutex);
  while (!state_->mapping_complete) {
    lock.unlock();
    state_->instance.ProcessEvents();
    state_->device.Tick();
    lock.lock();
  }
  if (state_->consumed) return false;
  state_->consumed = true;
  return true;
}

bool TextureReadFuture::CopyAndRelease(uint8_t* dst) {
  bool ok = false;
  if (state_->mapping_success) {
    const void* mapped_data =
        state_->staging_buffer.GetConstMappedRange(0, state_->buffer_size);
    if (mapped_data) {
      std::memcpy(dst, mapped_data, state_->buffer_size);
      ok = true;
    }
    state_->staging_buffer.Unmap();
  } else {
    spdlog::error("Texture readback mapping failed");
  }
  state_->staging_buffer.Destroy();
  state_.reset();
  return ok;
}

CpuImage TextureReadFuture::Await() {
  if (!WaitAndConsume()) return CpuImage();

  // Note: per-frame Await allocates state_->buffer_size bytes via
  // std::vector::resize (zero-initialized). For large frames this is
  // expensive (~33ms/frame for 1600x1200 RGBA8 in debug). Callers in
  // hot paths should use AwaitInto() with a reusable target instead.
  CpuImage result(state_->width, state_->height, state_->format);
  result.Resize(state_->width, state_->height, state_->aligned_row_pitch);
  CopyAndRelease(result.GetData());
  return result;
}

bool TextureReadFuture::AwaitInto(CpuImage& target) {
  if (!WaitAndConsume()) return false;

  target.Resize(state_->width, state_->height, state_->aligned_row_pitch,
                state_->format);
  return CopyAndRelease(target.GetData());
}

// TextureReadback implementation

TextureReadback::TextureReadback(wgpu::Instance instance, wgpu::Device device,
                                 wgpu::Queue queue)
    : instance_(instance), device_(device), queue_(queue) {}

TextureReadback::~TextureReadback() = default;

TextureReadFuture TextureReadback::ReadTextureMip(wgpu::Texture texture,
                                                  uint32_t mip_level,
                                                  uint32_t array_layer) {
  wgpu::TextureFormat format = texture.GetFormat();
  uint32_t width = std::max(1u, texture.GetWidth() >> mip_level);
  uint32_t height = std::max(1u, texture.GetHeight() >> mip_level);

  size_t bytes_per_pixel = GetBytesPerPixel(format);
  if (bytes_per_pixel == 0) {
    spdlog::error("Unsupported texture format for readback");
    return TextureReadFuture(nullptr);
  }

  size_t aligned_row_pitch = CalculateAlignedRowPitch(width, bytes_per_pixel);
  size_t buffer_size = aligned_row_pitch * height;

  // Create staging buffer
  wgpu::BufferDescriptor buffer_desc;
  buffer_desc.size = buffer_size;
  buffer_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer staging_buffer = device_.CreateBuffer(&buffer_desc);

  // Create command encoder
  wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();

  // Copy texture to buffer
  wgpu::TexelCopyTextureInfo src;
  src.texture = texture;
  src.mipLevel = mip_level;
  src.origin = {0, 0, array_layer};
  src.aspect = IsDepthFormat(format) ? wgpu::TextureAspect::DepthOnly
                                     : wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferInfo dst;
  dst.buffer = staging_buffer;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = static_cast<uint32_t>(aligned_row_pitch);
  dst.layout.rowsPerImage = height;

  wgpu::Extent3D extent = {width, height, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);

  // Submit command
  wgpu::CommandBuffer commands = encoder.Finish();
  queue_.Submit(1, &commands);

  // Create shared state for the future
  auto state = std::make_shared<TextureReadFuture::State>();
  state->instance = instance_;
  state->device = device_;
  state->staging_buffer = staging_buffer;
  state->width = width;
  state->height = height;
  state->aligned_row_pitch = aligned_row_pitch;
  state->buffer_size = buffer_size;
  state->format = format;

  // Start async mapping - callback signals completion via the state mutex.
  // Use AllowProcessEvents so the callback is delivered from
  // instance.ProcessEvents() that the polling loop in WaitAndConsume() pumps.
  // AllowSpontaneous has been observed to deliver mapping_success=true while
  // the staging buffer still holds zeros — the callback fires on a worker
  // thread before the queue.Submit copy retires.
  std::weak_ptr<TextureReadFuture::State> weak_state = state;
  staging_buffer.MapAsync(
      wgpu::MapMode::Read, 0, buffer_size,
      wgpu::CallbackMode::AllowProcessEvents,
      [weak_state](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (auto s = weak_state.lock()) {
          std::lock_guard<std::mutex> lock(s->mutex);
          s->mapping_complete = true;
          s->mapping_success = (status == wgpu::MapAsyncStatus::Success);
        }
      });

  return TextureReadFuture(std::move(state));
}

TextureReadFuture TextureReadback::ReadTexture(wgpu::Texture texture) {
  return ReadTextureMip(texture, 0, 0);
}

CpuImage TextureReadback::ReadTextureSync(wgpu::Texture texture, uint32_t width,
                                          uint32_t height,
                                          wgpu::TextureFormat format) {
  // Convenience wrapper - just uses the async API and awaits immediately
  (void)width;  // Width/height are computed from texture in ReadTexture
  (void)height;
  (void)format;
  return ReadTexture(texture).Await();
}

TextureReadback::RecordedCopy TextureReadback::RecordCopy(
    wgpu::CommandEncoder& encoder, wgpu::Texture texture, uint32_t mip_level,
    uint32_t array_layer) {
  wgpu::TextureFormat format = texture.GetFormat();
  uint32_t width = std::max(1u, texture.GetWidth() >> mip_level);
  uint32_t height = std::max(1u, texture.GetHeight() >> mip_level);

  size_t bytes_per_pixel = GetBytesPerPixel(format);
  if (bytes_per_pixel == 0) {
    spdlog::error("Unsupported texture format for readback");
    return {};
  }

  size_t aligned_row_pitch = CalculateAlignedRowPitch(width, bytes_per_pixel);
  size_t buffer_size = aligned_row_pitch * height;

  // Create staging buffer
  wgpu::BufferDescriptor buffer_desc;
  buffer_desc.size = buffer_size;
  buffer_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer staging_buffer = device_.CreateBuffer(&buffer_desc);

  // Record copy command
  wgpu::TexelCopyTextureInfo src;
  src.texture = texture;
  src.mipLevel = mip_level;
  src.origin = {0, 0, array_layer};
  src.aspect = IsDepthFormat(format) ? wgpu::TextureAspect::DepthOnly
                                     : wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferInfo dst;
  dst.buffer = staging_buffer;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = static_cast<uint32_t>(aligned_row_pitch);
  dst.layout.rowsPerImage = height;

  wgpu::Extent3D extent = {width, height, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);

  // Create shared state
  auto state = std::make_shared<TextureReadFuture::State>();
  state->instance = instance_;
  state->device = device_;
  state->staging_buffer = staging_buffer;
  state->width = width;
  state->height = height;
  state->aligned_row_pitch = aligned_row_pitch;
  state->buffer_size = buffer_size;
  state->format = format;

  return {.state = state,
          .staging_buffer = staging_buffer,
          .buffer_size = buffer_size};
}

TextureReadFuture TextureReadback::StartMapping(RecordedCopy copy) {
  if (!copy.state) {
    return TextureReadFuture(nullptr);
  }

  // See note on ReadTextureMip about callback mode: AllowProcessEvents pairs
  // with the ProcessEvents pump in WaitAndConsume().
  std::weak_ptr<TextureReadFuture::State> weak_state = copy.state;
  copy.staging_buffer.MapAsync(
      wgpu::MapMode::Read, 0, copy.buffer_size,
      wgpu::CallbackMode::AllowProcessEvents,
      [weak_state](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (auto s = weak_state.lock()) {
          std::lock_guard<std::mutex> lock(s->mutex);
          s->mapping_complete = true;
          s->mapping_success = (status == wgpu::MapAsyncStatus::Success);
        }
      });

  return TextureReadFuture(std::move(copy.state));
}

}  // namespace badlands
