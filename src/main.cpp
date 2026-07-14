#include <SDL3/SDL.h>

#include "engine/rendering/gpu_context.hpp"

int main(int, char**) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window* window = SDL_CreateWindow("badlands", 1600, 900, SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  badlands::GpuContext gpu;
  if (!gpu.Initialize(window)) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) running = false;
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        gpu.Configure(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
      }
    }

    wgpu::TextureView view = gpu.AcquireSurfaceTexture();
    if (!view) continue;

    wgpu::CommandEncoderDescriptor encoder_desc = {};
    wgpu::CommandEncoder encoder = gpu.GetDevice().CreateCommandEncoder(&encoder_desc);

    wgpu::RenderPassColorAttachment attachment = {};
    attachment.view = view;
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {0.02, 0.02, 0.03, 1.0};

    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &attachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
    pass.End();

    wgpu::CommandBufferDescriptor cmd_desc = {};
    wgpu::CommandBuffer commands = encoder.Finish(&cmd_desc);
    gpu.GetQueue().Submit(1, &commands);

    gpu.Present();
  }

  gpu.Shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
