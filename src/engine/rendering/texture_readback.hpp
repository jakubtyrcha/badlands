#pragma once

#include <memory>
#include <mutex>

#include <dawn/webgpu_cpp.h>

#include "core/util/cpu_image.hpp"

namespace badlands {

// Forward declaration
class TextureReadback;

// A future representing an in-progress texture readback.
// Call Await() to block until the read completes and get the result.
class TextureReadFuture {
 public:
  TextureReadFuture() = default;
  TextureReadFuture(TextureReadFuture&&) noexcept;
  TextureReadFuture& operator=(TextureReadFuture&&) noexcept;
  ~TextureReadFuture();

  // Non-copyable
  TextureReadFuture(const TextureReadFuture&) = delete;
  TextureReadFuture& operator=(const TextureReadFuture&) = delete;

  // Block until the readback completes and return the result.
  // Polls Dawn processEvents() until mapping completes.
  // Can only be called once - subsequent calls return empty CpuImage.
  CpuImage Await();

  // Block until mapping completes, then copy into an existing CpuImage.
  // Reuses the target's allocation if it has sufficient capacity,
  // avoiding the cost of zero-initializing a fresh buffer each frame.
  // Returns true on success.
  bool AwaitInto(CpuImage& target);

  // Check if the readback has completed (non-blocking)
  bool IsReady() const;

 private:
  friend class TextureReadback;

  bool WaitAndConsume();
  bool CopyAndRelease(uint8_t* dst);

  struct State {
    wgpu::Instance instance;
    wgpu::Device device;
    wgpu::Buffer staging_buffer;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t aligned_row_pitch = 0;
    size_t buffer_size = 0;
    wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;

    std::mutex mutex;
    bool mapping_complete = false;
    bool mapping_success = false;
    bool consumed = false;
  };

  explicit TextureReadFuture(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

// Reads GPU textures back to CPU memory asynchronously.
// Returns futures that can be awaited when the result is needed.
class TextureReadback {
 public:
  TextureReadback(wgpu::Instance instance, wgpu::Device device,
                  wgpu::Queue queue);
  ~TextureReadback();

  // Non-copyable, movable
  TextureReadback(const TextureReadback&) = delete;
  TextureReadback& operator=(const TextureReadback&) = delete;
  TextureReadback(TextureReadback&&) = default;
  TextureReadback& operator=(TextureReadback&&) = default;

  // Start async texture read, returns a future to await on.
  // The GPU copy is submitted immediately; Await() blocks until mapping
  // completes.
  TextureReadFuture ReadTextureMip(wgpu::Texture texture, uint32_t mip_level,
                                   uint32_t array_layer = 0);

  // Overload for base mip level (convenience)
  TextureReadFuture ReadTexture(wgpu::Texture texture);

  // Blocking read - convenience wrapper for ReadTexture().Await()
  // Use when you need the result immediately and don't need async.
  CpuImage ReadTextureSync(wgpu::Texture texture, uint32_t width,
                           uint32_t height, wgpu::TextureFormat format);

  // Record a texture-to-buffer copy onto an existing encoder (no submit).
  // Returns a future whose mapping must be started after the encoder is
  // submitted, via StartMapping().
  struct RecordedCopy {
    std::shared_ptr<TextureReadFuture::State> state;
    wgpu::Buffer staging_buffer;
    size_t buffer_size = 0;
  };
  RecordedCopy RecordCopy(wgpu::CommandEncoder& encoder, wgpu::Texture texture,
                          uint32_t mip_level = 0, uint32_t array_layer = 0);

  // Start async mapping on a previously recorded copy. Returns the future.
  TextureReadFuture StartMapping(RecordedCopy copy);

 private:
  wgpu::Instance instance_;
  wgpu::Device device_;
  wgpu::Queue queue_;
};

// Calculate row pitch aligned to 256 bytes (WebGPU requirement)
size_t CalculateAlignedRowPitch(uint32_t width, uint32_t bytes_per_pixel);

}  // namespace badlands
