#pragma once

// Task S2.A1: game-agnostic app framework. See sdl_viewer_app.hpp for the
// framework overview.

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuPipelineGenerator;
class SceneRenderer;

// Everything a view needs to build its scene + materials. Non-owning handles;
// all outlive the view (owned by SdlViewerApp).
struct RenderContext {
  wgpu::Device device;
  wgpu::Queue queue;
  GpuPipelineGenerator* pipeline_gen = nullptr;
  SceneRenderer* scene_renderer = nullptr;  // shared renderer, owned by the app
  wgpu::TextureFormat surface_format = wgpu::TextureFormat::BGRA8Unorm;
};

}  // namespace badlands
