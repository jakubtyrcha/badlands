#include "engine/app/screenshot.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "badlands_assets.h"
#include "engine/app/app_view.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

namespace {

// 256-byte row alignment WebGPU requires for CopyTextureToBuffer
// destinations (WGPU_COPY_BYTES_PER_ROW_ALIGNMENT).
constexpr uint32_t kCopyBytesPerRowAlignment = 256;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace

wgpu::Texture CreateScreenshotTarget(wgpu::Device device, uint32_t width,
                                     uint32_t height) {
  wgpu::TextureDescriptor offscreen_desc;
  offscreen_desc.size = {width, height, 1};
  offscreen_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  offscreen_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  offscreen_desc.mipLevelCount = 1;
  offscreen_desc.sampleCount = 1;
  offscreen_desc.dimension = wgpu::TextureDimension::e2D;
  return device.CreateTexture(&offscreen_desc);
}

bool WriteTextureToPng(wgpu::Device device, wgpu::Queue queue,
                       const wgpu::Texture& texture, uint32_t width,
                       uint32_t height, const std::string& path) {
  // Read the offscreen texture back: CopyTextureToBuffer requires
  // bytesPerRow to be a multiple of 256, which the tightly-packed RGBA8
  // buffer badlands_write_png expects generally isn't -- strip the padding
  // per row after mapping.
  const uint32_t unpadded_bytes_per_row = width * 4;
  const uint32_t padded_bytes_per_row =
      AlignUp(unpadded_bytes_per_row, kCopyBytesPerRowAlignment);
  const uint64_t buffer_size =
      static_cast<uint64_t>(padded_bytes_per_row) * height;

  wgpu::BufferDescriptor readback_desc;
  readback_desc.label = "screenshot readback";
  readback_desc.size = buffer_size;
  readback_desc.usage =
      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback_buffer = device.CreateBuffer(&readback_desc);

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::TexelCopyTextureInfo copy_src;
  copy_src.texture = texture;
  copy_src.mipLevel = 0;
  copy_src.origin = {0, 0, 0};
  copy_src.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferInfo copy_dst;
  copy_dst.buffer = readback_buffer;
  copy_dst.layout.offset = 0;
  copy_dst.layout.bytesPerRow = padded_bytes_per_row;
  copy_dst.layout.rowsPerImage = height;

  wgpu::Extent3D copy_size = {width, height, 1};
  encoder.CopyTextureToBuffer(&copy_src, &copy_dst, &copy_size);

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  bool map_done = false;
  wgpu::MapAsyncStatus map_status = wgpu::MapAsyncStatus::Error;
  readback_buffer.MapAsync(
      wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowSpontaneous,
      [&map_done, &map_status](wgpu::MapAsyncStatus status, wgpu::StringView) {
        map_status = status;
        map_done = true;
      });
  while (!map_done) SDL_Delay(5);

  if (map_status != wgpu::MapAsyncStatus::Success) {
    spdlog::error("WriteTextureToPng: buffer map failed (status={})",
                  static_cast<int>(map_status));
    return false;
  }

  const auto* mapped = static_cast<const uint8_t*>(
      readback_buffer.GetConstMappedRange(0, buffer_size));
  if (!mapped) {
    spdlog::error("WriteTextureToPng: GetConstMappedRange returned null");
    readback_buffer.Unmap();
    return false;
  }

  std::vector<uint8_t> packed(static_cast<size_t>(unpadded_bytes_per_row) *
                              height);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(packed.data() + static_cast<size_t>(y) * unpadded_bytes_per_row,
               mapped + static_cast<size_t>(y) * padded_bytes_per_row,
               unpadded_bytes_per_row);
  }
  readback_buffer.Unmap();

  badlands_write_png(path.c_str(), packed.data(), width, height);
  spdlog::info("WriteTextureToPng: wrote {} ({}x{})", path, width, height);
  return true;
}

bool SaveScreenshot(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t width, uint32_t height,
                    const std::string& path, GBufferDebugMode debug_mode) {
  wgpu::Device device = gpu.GetDevice();
  wgpu::Queue queue = gpu.GetQueue();

  wgpu::Texture offscreen_texture = CreateScreenshotTarget(device, width, height);
  wgpu::TextureView offscreen_view = offscreen_texture.CreateView();
  if (!offscreen_view) {
    spdlog::error("SaveScreenshot: failed to create offscreen texture");
    return false;
  }

  SceneRenderer renderer;
  renderer.Initialize(device, queue, &pipeline_gen,
                      wgpu::TextureFormat::RGBA8Unorm, width, height);
  renderer.SetDebugMode(debug_mode);

  view.Update(0.0f, SDL_GetKeyboardState(nullptr));
  renderer.Render(view.GetCamera(), view.GetRegistry(), view.GetSceneContext(),
                  offscreen_view);

  return WriteTextureToPng(device, queue, offscreen_texture, width, height,
                           path);
}

}  // namespace badlands
