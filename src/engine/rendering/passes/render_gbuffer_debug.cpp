// G-buffer debug visualization pass (Task S2.B4). See render_gbuffer_debug.hpp.
#include "engine/rendering/passes/render_gbuffer_debug.hpp"

#include <array>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "engine/core/camera.hpp"  // UniformData (frame UBO size)
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

bool RenderGBufferDebug(RenderPassContext& pass, FrameContext& frame,
                        GpuPipelineGenerator& pipeline_gen,
                        wgpu::TextureFormat target_format,
                        wgpu::TextureView depth_view,
                        wgpu::TextureView normals_view,
                        wgpu::TextureView albedo_view,
                        wgpu::TextureView material_view,
                        wgpu::TextureView hdr_view,
                        wgpu::TextureView ao_view,
                        wgpu::TextureView ui_overlay_view,
                        GBufferDebugMode mode) {
  if (mode == GBufferDebugMode::None) {
    return false;
  }

  RenderPipelineDeclaration decl;
  decl.shader_path = "passes/gbuffer_debug";
  // vertex_layout defaults to VertexLayout::kFullscreen (fullscreen triangle
  // from @builtin(vertex_index); no vertex buffer, no depth attachment).

  RenderTargetFormats target_formats = {target_format};
  auto compiled = pipeline_gen.GetPipeline(decl, target_formats);
  if (!compiled) {
    spdlog::error(
        "RenderGBufferDebug: failed to compile gbuffer_debug pipeline");
    return false;
  }

  // Debug-mode selector uniform — a plain u32, matching
  // shaders/passes/gbuffer_debug.wesl's `var<uniform> debug_mode: u32`.
  const uint32_t mode_value = static_cast<uint32_t>(mode);
  wgpu::Buffer mode_buffer =
      frame.CreateUniformBuffer(sizeof(uint32_t), &mode_value);

  // Group 0: frame UBO @0, gbuffer depth/normals/albedo/material @1-4, lit
  // HDR @5, debug-mode uniform @6, GTAO AO @7, UI overlay @8 — matches
  // shaders/passes/gbuffer_debug.wesl's binding declarations.
  std::array<wgpu::BindGroupEntry, 9> entries{};
  entries[0].binding = 0;
  entries[0].buffer = frame.GetFrameUniformBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(UniformData);
  entries[1].binding = 1;
  entries[1].textureView = depth_view;
  entries[2].binding = 2;
  entries[2].textureView = normals_view;
  entries[3].binding = 3;
  entries[3].textureView = albedo_view;
  entries[4].binding = 4;
  entries[4].textureView = material_view;
  entries[5].binding = 5;
  entries[5].textureView = hdr_view;
  entries[6].binding = 6;
  entries[6].buffer = mode_buffer;
  entries[6].offset = 0;
  entries[6].size = sizeof(uint32_t);
  entries[7].binding = 7;
  entries[7].textureView = ao_view;
  entries[8].binding = 8;
  entries[8].textureView = ui_overlay_view;

  wgpu::BindGroup bind_group =
      frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.Draw(3);
  return true;
}

}  // namespace badlands
