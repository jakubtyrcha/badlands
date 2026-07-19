#pragma once

// Forward render passes (ported from sampo's rendering/passes/render_forward,
// namespace sampo -> badlands; adapted to badlands' lazy vertex upload as in
// render_textured_mesh.cpp). Forward materials bypass the G-buffer and write
// color directly into the HDR target after deferred lighting; the material
// itself does any lighting. Two passes:
//   - RenderForwardMeshes:            entities tagged ForwardOpaqueRenderable
//   - RenderForwardTransparentMeshes: entities tagged ForwardTransparentRenderable
//
// Both draw StaticTexturedMeshComponent entities carrying a
// MaterialFactoryComponent, resolving instances via the shared cache with
// RenderPassType::kForward.

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <dawn/webgpu_cpp.h>

namespace badlands {

class RenderPassContext;
class FrameContext;
class MaterialInstanceCache;

// Engine-owned resources bound at @group(2) for forward-transparent materials
// that declare them (e.g. the water surface): scene depth (read-only), a copy
// of the HDR scene color (for normal-driven distortion/refraction), and the IBL
// prefiltered environment + BRDF LUT (for reflections). Built once and reused
// for every transparent draw. A transparent material whose pipeline has no
// group-2 layout must leave `scene_depth` null so the pass skips the group-2
// bind. `time_seconds` is injected into the material's per-object `time`
// parameter (group 1) so wave animation is engine-driven; screen size comes
// from the frame UBO.
struct ForwardEngineResources {
  wgpu::TextureView scene_depth;
  wgpu::TextureView scene_color;
  wgpu::Sampler scene_color_sampler;
  wgpu::TextureView ibl_prefiltered;
  wgpu::Sampler ibl_sampler;
  wgpu::TextureView brdf_lut;
  wgpu::Sampler brdf_lut_sampler;
  wgpu::TextureView shadow_map;      // sun shadow map (texture_depth_2d)
  wgpu::Sampler shadow_sampler;      // comparison sampler (LessEqual)
  float time_seconds{0.0f};
};

// Draw ForwardOpaqueRenderable textured meshes (no engine group-2 resources).
void RenderForwardMeshes(RenderPassContext& pass, FrameContext& frame,
                         entt::registry& registry,
                         const glm::vec3& camera_world_pos,
                         MaterialInstanceCache& cache);

// Draw ForwardTransparentRenderable textured meshes. When `engine.scene_depth`
// is set, the engine resources are bound at @group(2) for each draw.
void RenderForwardTransparentMeshes(RenderPassContext& pass, FrameContext& frame,
                                    entt::registry& registry,
                                    const glm::vec3& camera_world_pos,
                                    MaterialInstanceCache& cache,
                                    const ForwardEngineResources& engine);

}  // namespace badlands
