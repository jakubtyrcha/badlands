#include "engine/tests/buffer_readback.hpp"

namespace badlands::test {

std::vector<uint8_t> ReadBufferSyncBytes(wgpu::Instance instance,
                                         wgpu::Device device, wgpu::Queue queue,
                                         wgpu::Buffer src, uint64_t offset,
                                         uint64_t size) {
  if (size == 0) return {};

  wgpu::BufferDescriptor staging_desc{};
  staging_desc.size = size;
  staging_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer staging = device.CreateBuffer(&staging_desc);

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  encoder.CopyBufferToBuffer(src, offset, staging, 0, size);
  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  bool map_done = false;
  bool map_ok = false;
  staging.MapAsync(
      wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowProcessEvents,
      [&map_done, &map_ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
        map_ok = (status == wgpu::MapAsyncStatus::Success);
        map_done = true;
      });

  // Blocking wait: pump both the instance (delivers the MapAsync callback)
  // and the device (drives queue submission / internal ticking) until the
  // callback above fires. Mirrors gpu_test_helpers.hpp's WaitForGpu loop.
  while (!map_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  std::vector<uint8_t> result;
  if (map_ok) {
    const void* mapped = staging.GetConstMappedRange(0, size);
    if (mapped) {
      result.resize(size);
      std::memcpy(result.data(), mapped, size);
    }
    staging.Unmap();
  }
  return result;
}

}  // namespace badlands::test
