// Skybox background pass (Task S2.B3). See render_skybox.hpp.
//
// Built exactly like scene_renderer.cpp's inline deferred-lighting/tonemap
// fullscreen passes: GetPipeline (explicit reflection-derived bind-group
// layouts, not Dawn AUTO) + frame.CreateBindGroup + a 3-vertex Draw. Kept as a
// standalone pass function (rather than inlined) so the SceneRenderer's Render
// sequence reads as G-buffer -> skybox -> deferred -> tonemap.
#include "engine/rendering/passes/render_skybox.hpp"

#include <array>

#include <spdlog/spdlog.h>

#include "engine/core/camera.hpp"  // UniformData (frame UBO size)
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

bool RenderSkybox(RenderPassContext& pass, FrameContext& frame,
                  GpuPipelineGenerator& pipeline_gen,
                  wgpu::TextureFormat target_format, wgpu::TextureView env_cube,
                  wgpu::Sampler env_sampler) {
  if (!env_cube || !env_sampler) {
    return false;
  }

  RenderPipelineDeclaration decl;
  decl.shader_path = "passes/skybox";
  // vertex_layout defaults to VertexLayout::kFullscreen (fullscreen triangle
  // from @builtin(vertex_index); no vertex buffer, no depth attachment).

  RenderTargetFormats target_formats = {target_format};
  auto compiled = pipeline_gen.GetPipeline(decl, target_formats);
  if (!compiled) {
    spdlog::error("RenderSkybox: failed to compile skybox pipeline");
    return false;
  }

  // Group 0: frame UBO @0, environment cube @1, its sampler @2 — matches
  // shaders/passes/skybox.wesl's binding declarations.
  std::array<wgpu::BindGroupEntry, 3> entries{};
  entries[0].binding = 0;
  entries[0].buffer = frame.GetFrameUniformBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(UniformData);
  entries[1].binding = 1;
  entries[1].textureView = env_cube;
  entries[2].binding = 2;
  entries[2].sampler = env_sampler;

  wgpu::BindGroup bind_group =
      frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.Draw(3);
  return true;
}

}  // namespace badlands
