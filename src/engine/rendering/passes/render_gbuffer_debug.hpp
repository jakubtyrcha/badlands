#pragma once

// G-buffer debug visualization pass (Task S2.B4). Fullscreen blit that
// visualizes a single G-buffer channel (or a passthrough of the lit HDR
// buffer) — see shaders/passes/gbuffer_debug.wesl. Built exactly like
// render_skybox.cpp's standalone pass function: GetPipeline (explicit
// reflection-derived bind-group layout) + frame.CreateBindGroup + a 3-vertex
// Draw. SceneRenderer::Render calls this INSTEAD OF the tonemap pass when
// GetDebugMode() != GBufferDebugMode::None.
#include <dawn/webgpu_cpp.h>

#include "engine/rendering/scene_renderer.hpp"  // GBufferDebugMode

namespace badlands {

class FrameContext;
class RenderPassContext;
class GpuPipelineGenerator;

// Compiles (once, cached) the "passes/gbuffer_debug" pipeline via
// `pipeline_gen` for a single color target of `target_format`, binds frame
// UBO @0 + the G-buffer depth/normals/albedo/material views @1-4 + the lit
// HDR view @5 + a debug-mode uniform @6 (matches
// shaders/passes/gbuffer_debug.wesl's binding declarations), and draws the
// fullscreen triangle selecting `mode`'s channel.
//
// Preconditions: `pass` must already be begun with exactly one color
// attachment of format `target_format` and NO depth attachment (matches the
// fullscreen gbuffer_debug pipeline: VertexLayout::kFullscreen, depth_format
// Undefined). Returns false (drawing nothing) if `mode` is
// GBufferDebugMode::None or the pipeline fails to compile.
bool RenderGBufferDebug(RenderPassContext& pass, FrameContext& frame,
                        GpuPipelineGenerator& pipeline_gen,
                        wgpu::TextureFormat target_format,
                        wgpu::TextureView depth_view,
                        wgpu::TextureView normals_view,
                        wgpu::TextureView albedo_view,
                        wgpu::TextureView material_view,
                        wgpu::TextureView hdr_view, GBufferDebugMode mode);

}  // namespace badlands
