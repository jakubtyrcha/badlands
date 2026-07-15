// Shared headless GPU helpers for test files.
// Provides RequestAdapter, RequestDevice, and WaitForGpu for GPU test
// executables. Ported from sampo/tests/gpu_test_helpers.hpp.
#pragma once

#include <dawn/webgpu_cpp.h>

#include <iostream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

namespace badlands::test {

// Request adapter (headless — no surface). Event-driven: the callback fires
// during `instance.ProcessEvents()`, which we pump from this thread until
// the request completes. No sleeps, no busy-wait.
inline wgpu::Adapter RequestAdapter(wgpu::Instance instance) {
  struct UserData {
    wgpu::Adapter adapter = nullptr;
    bool request_ended = false;
  };
  UserData user_data;

  wgpu::RequestAdapterOptions options;
  options.compatibleSurface = nullptr;

  instance.RequestAdapter(
      &options, wgpu::CallbackMode::AllowProcessEvents,
      [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
         wgpu::StringView message, UserData* data) {
        if (status == wgpu::RequestAdapterStatus::Success) {
          data->adapter = std::move(adapter);
        } else {
          std::cerr << "Could not get WebGPU adapter: "
                    << (message.length > 0
                            ? std::string(message.data, message.length)
                            : std::string("Unknown error"))
                    << std::endl;
        }
        data->request_ended = true;
      },
      &user_data);

  while (!user_data.request_ended) {
    instance.ProcessEvents();
  }

  return user_data.adapter;
}

// Request device with optional label. Event-driven via the adapter's
// associated instance — same pattern as RequestAdapter.
inline wgpu::Device RequestDevice(wgpu::Adapter adapter,
                                  const char* label = "Test Device") {
  struct UserData {
    wgpu::Device device = nullptr;
    bool request_ended = false;
  };
  UserData user_data;

  wgpu::DeviceDescriptor device_desc;
  device_desc.label = label;

  // Surface Dawn validation / out-of-memory / internal errors to stderr so
  // silent dropouts (e.g. an invalid texture-to-texture copy) don't show up
  // only as zeroed readback bytes downstream. Tests that intentionally provoke
  // errors should override this on a per-test basis.
  device_desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        std::cerr << "Dawn uncaptured error (type=" << static_cast<int>(type)
                  << "): "
                  << (message.length > 0
                          ? std::string(message.data, message.length)
                          : std::string("(no message)"))
                  << std::endl;
      });

  adapter.RequestDevice(
      &device_desc, wgpu::CallbackMode::AllowProcessEvents,
      [](wgpu::RequestDeviceStatus status, wgpu::Device device,
         wgpu::StringView message, UserData* data) {
        if (status == wgpu::RequestDeviceStatus::Success) {
          data->device = std::move(device);
        } else {
          std::cerr << "Could not get WebGPU device: "
                    << (message.length > 0
                            ? std::string(message.data, message.length)
                            : std::string("Unknown error"))
                    << std::endl;
        }
        data->request_ended = true;
      },
      &user_data);

  wgpu::Instance instance = adapter.GetInstance();
  while (!user_data.request_ended) {
    instance.ProcessEvents();
  }

  return user_data.device;
}

// Create a small RGBA8Unorm texture filled with `pixels` (row-major, w*h*4
// bytes). Issues a synchronous queue.WriteTexture; caller is responsible for
// driving GPU progress (e.g. via WaitForGpu) before sampling.
inline wgpu::Texture CreateRgbaTexture(const wgpu::Device& device,
                                       const wgpu::Queue& queue, uint32_t width,
                                       uint32_t height,
                                       const std::vector<uint8_t>& pixels) {
  REQUIRE(pixels.size() == static_cast<size_t>(width) * height * 4u);
  wgpu::TextureDescriptor desc{};
  desc.size = {width, height, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  desc.mipLevelCount = 1;
  wgpu::Texture texture = device.CreateTexture(&desc);

  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = texture;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  wgpu::TexelCopyBufferLayout layout{};
  layout.offset = 0;
  layout.bytesPerRow = width * 4u;
  layout.rowsPerImage = height;
  wgpu::Extent3D extent = {width, height, 1};
  queue.WriteTexture(&dst, pixels.data(), pixels.size(), &layout, &extent);
  return texture;
}

// Convenience: 16x16 RGBA8Unorm texture with four solid-color quadrants.
//   top-left  = red    (255,   0,   0, 255)
//   top-right = green  (  0, 255,   0, 255)
//   bot-left  = blue   (  0,   0, 255, 255)
//   bot-right = yellow (255, 255,   0, 255)
inline wgpu::Texture CreateQuadrantTexture(const wgpu::Device& device,
                                           const wgpu::Queue& queue) {
  constexpr uint32_t kSize = 16;
  std::vector<uint8_t> pixels(kSize * kSize * 4u);
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      uint8_t* p = pixels.data() + (y * kSize + x) * 4u;
      const bool right = x >= kSize / 2u;
      const bool bottom = y >= kSize / 2u;
      if (!bottom && !right) {  // top-left red
        p[0] = 255; p[1] = 0;   p[2] = 0;   p[3] = 255;
      } else if (!bottom && right) {  // top-right green
        p[0] = 0;   p[1] = 255; p[2] = 0;   p[3] = 255;
      } else if (bottom && !right) {  // bot-left blue
        p[0] = 0;   p[1] = 0;   p[2] = 255; p[3] = 255;
      } else {  // bot-right yellow
        p[0] = 255; p[1] = 255; p[2] = 0;   p[3] = 255;
      }
    }
  }
  return CreateRgbaTexture(device, queue, kSize, kSize, pixels);
}

// Wait for all submitted GPU work to complete
inline void WaitForGpu(wgpu::Instance instance, wgpu::Device device,
                       wgpu::Queue queue) {
  bool work_done = false;
  queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::AllowProcessEvents,
      [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
        work_done = true;
        REQUIRE(status == wgpu::QueueWorkDoneStatus::Success);
      });

  while (!work_done) {
    instance.ProcessEvents();
    device.Tick();
  }
}

}  // namespace badlands::test
