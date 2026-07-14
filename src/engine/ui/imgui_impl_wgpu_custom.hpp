#pragma once

// Ported from sampo's src/ui/imgui_impl_wgpu_custom.{hpp,cpp} (Task S2.A2).
// Custom Dear ImGui Dawn/WebGPU renderer backend. Standard ImGui backend
// naming/API shape (ImGui_ImplWGPU_*, global scope) -- intentionally not
// wrapped in the badlands namespace, matching every other ImGui backend
// (e.g. imgui_impl_sdl3).

#include <imgui.h>

#include <dawn/webgpu_cpp.h>

struct ImGui_ImplWGPU_InitInfo {
  wgpu::Device Device;
  int NumFramesInFlight = 3;
  wgpu::TextureFormat RenderTargetFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureFormat DepthStencilFormat = wgpu::TextureFormat::Undefined;
  uint32_t FramebufferWidth = 0;
  uint32_t FramebufferHeight = 0;
  bool OutputIsLinear = false;  // true when rendering to RGBA16Float surface
};

bool ImGui_ImplWGPU_Init(ImGui_ImplWGPU_InitInfo* init_info);
// Init without SDL backend (for headless/test use). Creates ImGui context internally.
bool ImGui_ImplWGPU_InitHeadless(ImGui_ImplWGPU_InitInfo* init_info);
void ImGui_ImplWGPU_Shutdown();
void ImGui_ImplWGPU_NewFrame();
void ImGui_ImplWGPU_RenderDrawData(ImDrawData* draw_data,
                                   wgpu::RenderPassEncoder pass);
void ImGui_ImplWGPU_InvalidateDeviceObjects();
bool ImGui_ImplWGPU_CreateDeviceObjects();
void ImGui_ImplWGPU_SetFramebufferSize(uint32_t width, uint32_t height);
