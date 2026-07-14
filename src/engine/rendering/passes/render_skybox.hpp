#pragma once

// Skybox background pass (Task S2.B3). Draws the source environment cube as a
// fullscreen skybox into the current render pass's color target (the
// SceneRenderer's HDR accumulation target).
//
// This is the sampo-equivalent of src/rendering/passes/render_skybox, reshaped
// to badlands' explicit-reflection pipeline path: it reconstructs the per-pixel
// world-space view ray from the frame UBO (pure-rotation view + proj, camera at
// origin) and samples the cube with STANDARD cube sampling — see
// shaders/passes/skybox.wesl.
#include <dawn/webgpu_cpp.h>

namespace badlands {

class FrameContext;
class RenderPassContext;
class GpuPipelineGenerator;

// Compiles (once, cached) the "passes/skybox" pipeline via `pipeline_gen` for a
// single color target of `target_format`, binds frame UBO @0 + `env_cube` @1 +
// `env_sampler` @2, and draws the fullscreen triangle. Returns false (drawing
// nothing) if `env_cube`/`env_sampler` are null or the pipeline fails to
// compile.
//
// Preconditions: `pass` must already be begun with exactly one color
// attachment of format `target_format` and NO depth attachment (matches the
// fullscreen skybox pipeline: VertexLayout::kFullscreen, depth_format
// Undefined).
bool RenderSkybox(RenderPassContext& pass, FrameContext& frame,
                  GpuPipelineGenerator& pipeline_gen,
                  wgpu::TextureFormat target_format, wgpu::TextureView env_cube,
                  wgpu::Sampler env_sampler);

}  // namespace badlands
