#pragma once

// Generic, game-agnostic post-scene hook. The SceneRenderer invokes a single
// registered ScenePostPass once per frame, late in the HDR pipeline (after the
// forward-transparent pass, before debug lines and tonemap), giving the pass a
// readable snapshot of the lit HDR colour, the write target, and the G-buffer
// depth. It is the seam a game uses to modulate the whole scene in world space
// (e.g. fog-of-war): reconstruct world position from depth, sample a
// world-space texture the game owns, and write the modulated colour.
//
// The engine knows nothing about what the pass does — it only snapshots the HDR
// colour and calls Execute(). The pass owns its own pipeline/shader/textures
// and records into the provided FrameContext. No game types appear here.

#include <cstdint>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class FrameContext;
class Camera;

// Everything a post-scene pass needs to run one fullscreen modulation pass.
struct PostSceneContext {
  FrameContext& frame;              // frame UBO + CreateBindGroup + BeginRenderPass
  const Camera& camera;             // for any camera-derived params the pass wants
  wgpu::TextureView color_source;   // readable HDR snapshot (pre-tonemap)
  wgpu::TextureView color_target;   // HDR target to write (same size/format as source)
  wgpu::TextureView depth;          // G-buffer depth (reversed-Z), for world reconstruction
  uint32_t width = 0;
  uint32_t height = 0;
  wgpu::TextureFormat color_format = wgpu::TextureFormat::Undefined;  // = accumulation format
};

// Interface for a game-supplied post-scene modulation pass.
class ScenePostPass {
 public:
  virtual ~ScenePostPass() = default;
  // Record the pass. Open a render pass on ctx.color_target, sample
  // ctx.color_source / ctx.depth, and draw. Must not touch anything else.
  virtual void Execute(const PostSceneContext& ctx) = 0;
};

}  // namespace badlands
