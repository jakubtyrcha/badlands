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
class RenderingMaterialInstance;

// Engine-owned resources for forward (opaque or transparent) materials that
// declare @group(2). The two passes bind different subsets:
//   - RenderForwardMeshes (opaque): shadow_map/shadow_sampler +
//     ibl_prefiltered/ibl_sampler + brdf_lut/brdf_lut_sampler only. Never
//     scene_depth/scene_color — scene_depth is also the opaque pass's own
//     writable depth attachment, and scene_color is stale at opaque time.
//   - RenderForwardTransparentMeshes (e.g. the water surface): all fields,
//     including scene_depth (read-only) and a copy of the HDR scene_color
//     (for normal-driven distortion/refraction).
// Built once and reused for every draw in a pass. `time_seconds` is injected
// into the material's per-object `time` parameter (group 1) so wave animation
// is engine-driven; screen size comes from the frame UBO.
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

// Build the @group(2) engine bind group for the forward-OPAQUE pass: shadow
// map + IBL only (6 entries: shadow_map@0/shadow_sampler@1 +
// ibl_prefiltered@2/ibl_sampler@3 + brdf_lut@4/brdf_lut_sampler@5). No
// scene_depth/scene_color (scene_depth is also the opaque pass's writable
// depth attachment; scene_color is stale at opaque time) — see
// RenderForwardMeshes below. Hoisted here (definition stays in
// render_forward.cpp) so InstancedMeshField (instanced_mesh_field.cpp) can
// build the identical group for its forward-opaque submeshes without
// duplicating the entry layout. Caller must have already checked
// `instance->DeclaresBindGroup(2)` and the same availability gate
// RenderForwardMeshes uses (shadow_map && ibl_prefiltered && brdf_lut).
wgpu::BindGroup BuildForwardOpaqueEngineBindGroup(RenderingMaterialInstance* instance,
                                                  FrameContext& frame,
                                                  const ForwardEngineResources& engine);

// True iff `engine` carries every resource the forward-opaque @group(2) bind
// actually consumes (shadow_map && ibl_prefiltered && brdf_lut — never
// scene_depth/scene_color, see BuildForwardOpaqueEngineBindGroup above).
// Shared gate: RenderForwardMeshes below and InstancedMeshField::Draw
// (instanced_mesh_field.cpp, for its forward-opaque submeshes) both use this
// exact check before binding group 2, so a group-2-declaring material is
// never drawn with a required group 2 left unbound in either path.
bool ForwardOpaqueEngineAvailable(const ForwardEngineResources& engine);

// Draw ForwardOpaqueRenderable textured meshes. For each draw whose material
// declares @group(2), binds a purpose-fit 6-entry group (shadow map + IBL
// prefiltered env + BRDF LUT) built from `engine`. Availability is gated on
// those three resources (shadow_map && ibl_prefiltered && brdf_lut), not
// `engine.scene_depth`; if a group-2 material is drawn while they're
// unavailable, the entity is skipped rather than drawn with group 2 unbound.
void RenderForwardMeshes(RenderPassContext& pass, FrameContext& frame,
                         entt::registry& registry,
                         const glm::vec3& camera_world_pos,
                         MaterialInstanceCache& cache,
                         const ForwardEngineResources& engine);

// Draw ForwardTransparentRenderable textured meshes. When `engine.scene_depth`
// is set, the engine resources are bound at @group(2) for each draw.
void RenderForwardTransparentMeshes(RenderPassContext& pass, FrameContext& frame,
                                    entt::registry& registry,
                                    const glm::vec3& camera_world_pos,
                                    MaterialInstanceCache& cache,
                                    const ForwardEngineResources& engine);

}  // namespace badlands
