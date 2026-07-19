#pragma once

// Ported (reconciled) from sampo's
// src/rendering/passes/render_textured_mesh.{hpp,cpp}, namespace sampo ->
// badlands.
//
// Deviations from sampo's version:
// - Sampo's `RenderTexturedMeshes` renders the G-buffer/shadow variants and
//   explicitly skips `ForwardOpaqueRenderable` entities (sampo has a
//   separate `rendering/passes/render_forward.{hpp,cpp}` for those, not
//   ported here). Since badlands Stage 1 has no G-buffer/deferred-lighting
//   pass at all (see the trim note in scene_renderer.cpp), this port
//   repurposes sampo's `render_textured_mesh.cpp` iteration/upload/draw
//   logic to BE the forward-opaque mesh pass instead: it draws every
//   `StaticTexturedMeshComponent` + `MaterialFactoryComponent` entity
//   unconditionally (no forward/deferred skip check), exactly as the task
//   brief specifies. `MaterialFactoryComponent::pass_type` still selects the
//   correct `MeshRenderingMaterial` variant (opaque vs. blended
//   transparent) via `MaterialInstanceCache::GetOrCreate` — see
//   `standard_material_factory.cpp`'s `kVariants`.
// - `RenderConfig`/wireframe support is dropped: sampo's wireframe path
//   lazily builds a line-index buffer via `GenerateLineIndicesFromTriangles`
//   (`rendering/geometry/wireframe_utils.hpp`), not ported (out of scope —
//   no wireframe toggle exists anywhere in badlands yet).
// - GPU vertex-buffer upload writes into `StaticTexturedMeshGpuComponent`
//   (re-added minimally by this task in mesh_components.hpp) instead of
//   sampo's `StaticTexturedMeshGpuComponent` + `GpuResourceManager`
//   deferred-deletion hook — see that struct's deviation note.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material.hpp"

namespace badlands {

class FrameContext;
class MaterialInstanceCache;
class RenderPassContext;
struct Frustum;

// Renders every StaticTexturedMeshComponent entity that also has a
// MaterialFactoryComponent: lazily uploads the mesh's vertex buffer to the
// GPU on first draw (or after `StaticTexturedMeshComponent::dirty` is set
// again), resolves a RenderingMaterialInstance via `cache` (keyed by
// factory + geometry + render_pass_type + the component's texture/uniform
// config hash), sets the per-object `modelMatrix` (rebased to camera-offset
// space: world_pos - camera_world_pos), and draws.
//
// camera_world_pos: camera position in world space (for the model-matrix
// camera-offset rebase).
// render_pass_type: which compiled pipeline variant to fetch (this port only
// ever calls this with RenderPassType::kForward — see scene_renderer.cpp).
// `frustum`: world-space view frustum; entities with a StaticMeshAabbComponent
// whose world AABB falls fully outside it are skipped (chunk culling).
void RenderTexturedMeshes(RenderPassContext& pass, FrameContext& frame,
                          entt::registry& registry,
                          const glm::vec3& camera_world_pos,
                          RenderPassType render_pass_type,
                          MaterialInstanceCache& cache,
                          const Frustum& frustum);

}  // namespace badlands
