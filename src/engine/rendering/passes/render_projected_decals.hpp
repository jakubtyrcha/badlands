#pragma once

// Projected-decal pass: composites ProjectedDecal outlines into an HDR colour
// target by reconstructing world positions from the G-buffer depth.
//
// This is the COLOUR-stage injection of the decal system. The ProjectedDecal
// descriptor itself is stage-independent (see projected_decal.hpp), so a future
// pass can stamp the same decals into the G-buffer or the light buffer without
// touching the primitive.

#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

namespace badlands {

class FrameContext;
class GpuPipelineGenerator;
class GpuTimer;
struct ProjectedDecal;

// Records one fullscreen, alpha-blended pass drawing `count` decals into
// `color_target`. `depth` and `normals` are the G-buffer views (reversed-Z
// depth for world reconstruction, octahedron normals to keep ground decals off
// vertical surfaces). `time_seconds` drives the dash scroll -- the renderer
// feeds it the WALL-CLOCK time (SceneContext::real_time_seconds), since decals
// are UI-domain and must keep animating while the game is paused.
//
// Takes the GpuTimer itself rather than a PassTimestampWrites handle (the same
// arrangement as VolumetricFog::Render): BeginPass eagerly reserves a named
// query slot, so it must only be called once this function has committed to
// recording a pass. Passing a pre-made handle in would register a "decals" row
// that the early-outs below could then leave with never-written timestamps.
//
// `view_proj` is the WORLD -> clip matrix (camera.GetProj() * camera.GetView(),
// the same product the frustum culler uses) and `width`/`height` the target
// size: together they scissor the pass down to the screen region the decals can
// actually touch, so a couple of small highlights do not cost a full-screen
// shade. Correctness never depends on it -- an untrustworthy projection just
// shades everything.
//
// A no-op when `decals` is null, `count` is 0, the decals are entirely
// off-screen, or the pipeline fails to compile. Decals beyond
// kMaxProjectedDecals are dropped with a warning.
void RenderProjectedDecals(FrameContext& frame, GpuPipelineGenerator& generator,
                           wgpu::TextureFormat color_format,
                           wgpu::TextureView color_target,
                           wgpu::TextureView depth, wgpu::TextureView normals,
                           const ProjectedDecal* decals, uint32_t count,
                           const glm::mat4& view_proj, uint32_t width,
                           uint32_t height, float time_seconds,
                           GpuTimer& gpu_timer);

}  // namespace badlands
