#pragma once

// Blocking GPU buffer readback for Catch2 GPU tests. Modeled on the buffer
// readback pattern in engine/rendering/gpu_timer.cpp (staging MapRead|CopyDst
// buffer + encoder.CopyBufferToBuffer + MapAsync) and on the API shape of
// engine/rendering/texture_readback.hpp's TextureReadback, but BLOCKING (spins
// instance.ProcessEvents()/device.Tick() until the map callback fires) rather
// than returning a future — tests want the bytes immediately, synchronously,
// the same way TextureReadback::ReadTextureSync does for textures.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <dawn/webgpu_cpp.h>

namespace badlands::test {

// Copies `size` bytes starting at `offset` from `src` (usage must include
// CopySrc) to a fresh MapRead|CopyDst staging buffer, submits, then blocks
// until the mapping completes. Returns the bytes, or an empty vector if the
// mapping failed (e.g. Dawn validation rejected the copy).
std::vector<uint8_t> ReadBufferSyncBytes(wgpu::Instance instance,
                                         wgpu::Device device, wgpu::Queue queue,
                                         wgpu::Buffer src, uint64_t offset,
                                         uint64_t size);

// Typed convenience: reads `count * sizeof(T)` bytes starting at byte
// `offset` and reinterprets them as `T`. Returns a zero-length vector (NOT a
// vector of `count` zeroed elements) if the underlying byte read came back
// short, so callers can tell a failed readback apart from real zeroed data.
template <class T>
std::vector<T> ReadBufferSync(wgpu::Instance instance, wgpu::Device device,
                              wgpu::Queue queue, wgpu::Buffer src,
                              uint64_t offset, size_t count) {
  const uint64_t byte_size = static_cast<uint64_t>(count) * sizeof(T);
  std::vector<uint8_t> bytes =
      ReadBufferSyncBytes(instance, device, queue, src, offset, byte_size);
  if (bytes.size() < byte_size) return {};

  std::vector<T> result(count);
  std::memcpy(result.data(), bytes.data(), byte_size);
  return result;
}

}  // namespace badlands::test
