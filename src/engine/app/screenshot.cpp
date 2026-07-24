#include "engine/app/screenshot.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

#include <spdlog/spdlog.h>

#include "core/util/cpu_image.hpp"
#include "engine/app/app_view.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/ui/ui_renderer.hpp"

namespace badlands {

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

bool WriteTextureToPng(wgpu::Instance instance, wgpu::Device device,
                       wgpu::Queue queue, const wgpu::Texture& texture,
                       uint32_t width, uint32_t height,
                       const std::string& path) {
  // Read the offscreen texture back to a CpuImage (handles the 256-byte row
  // alignment + depadding), then write it as a PNG via the `assets` crate.
  // AwaitInto (not Await) so a failed buffer map is reported as false rather
  // than silently yielding a valid-dimension all-black image.
  TextureReadback readback(instance, device, queue);
  CpuImage image;
  if (!readback.ReadTexture(texture).AwaitInto(image)) {
    spdlog::error("WriteTextureToPng: texture readback failed for {}", path);
    return false;
  }

  if (image.GetWidth() != width || image.GetHeight() != height) {
    spdlog::error(
        "WriteTextureToPng: readback size {}x{}, expected {}x{}",
        image.GetWidth(), image.GetHeight(), width, height);
    return false;
  }

  if (!image.WritePng(path)) {
    spdlog::error("WriteTextureToPng: WritePng failed for {}", path);
    return false;
  }

  spdlog::info("WriteTextureToPng: wrote {} ({}x{})", path, width, height);
  return true;
}

bool SaveScreenshot(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t width, uint32_t height,
                    const std::string& path, GBufferDebugMode debug_mode,
                    ShadowDebugMode shadow_debug_mode, float time_of_day,
                    const ColorGradingConfig& grading) {
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
                      wgpu::TextureFormat::RGBA8Unorm, width, height,
                      gpu.HasR8UnormStorage());
  renderer.SetDebugMode(debug_mode);
  renderer.SetShadowDebugMode(shadow_debug_mode);
  // Grading is part of the game's image (unlike the P3 resolve, which stays
  // off here so profile-less PNGs remain sRGB-referred).
  renderer.SetColorGradingConfig(grading);

  view.SeekToTimeOfDay(time_of_day);  // deterministic time-of-day for the capture
  view.Update(0.0f, SDL_GetKeyboardState(nullptr));

  // Game UI is part of the game's image, so captures include it. (Dear ImGui
  // debug UI is NOT: this path never initializes ImGui.) Prepare before any
  // pass is open, and against the OFFSCREEN format -- RGBA8Unorm here vs the
  // window's BGRA8Unorm, so this builds its own pipeline variant.
  UiRenderer* ui = view.GetUiRenderer();
  if (ui) ui->Prepare(width, height, wgpu::TextureFormat::RGBA8Unorm);

  renderer.Render(view.GetCamera(), view.GetRegistry(), view.GetSceneContext(),
                  offscreen_view);

  // Overlay pass: mirrors the app's, loading the tonemapped result the renderer
  // just wrote and adding the UI on top with no depth attachment.
  if (ui) {
    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = offscreen_view;
    color_attachment.loadOp = wgpu::LoadOp::Load;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = nullptr;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    ui->Draw(pass);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    queue.Submit(1, &commands);
  }

  return WriteTextureToPng(gpu.GetInstance(), device, queue, offscreen_texture,
                           width, height, path);
}

}  // namespace badlands
