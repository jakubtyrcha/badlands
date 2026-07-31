#pragma once

// Reusable engine component bundling a GpuInstanceRenderer with a per-submesh
// {render-pass kind, material} mapping. Game-agnostic — no game/ECS types,
// no tree/leaf/foliage/bark vocabulary (see gpu_instance_renderer.hpp: this
// class is the "submesh" concept's actual consumer). A caller (e.g. a
// game-side field builder) resolves and owns whatever RenderingMaterialInstance
// it wants per (model, lod, submesh) and hands it in via SetSubmesh; this
// class never reaches into a material cache, scene, or ECS itself, mirroring
// GpuInstanceRenderer::BucketSubmeshMaterialFn's contract.
//
// Usage (see the design doc, §[C] SceneRenderer integration):
//   field.UploadInstances(...); field.SetSubmesh(...) per (model,lod,submesh);
//   each frame: field.Cull(frame, camera) BEFORE any render pass opens on the
//   same encoder (see GpuInstanceRenderer::Cull's sequencing contract), then
//   field.Draw(pass, frame, PassKind::kDeferred) inside the G-buffer pass and
//   field.Draw(pass, frame, PassKind::kForwardOpaque, &engine) inside the
//   forward-opaque pass. A field with shadow-casting submeshes (Phase 4:
//   SetSubmeshShadow attached at least one) additionally needs
//   field.CullShadow(frame, camera, light_view_proj) in the same pre-render-
//   pass window, and field.Draw(pass, frame, PassKind::kShadow) inside the
//   shadow depth pass.
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/gpu_instance_renderer.hpp"

namespace badlands {

class Camera;
class FrameContext;
class RenderPassContext;
class GpuPipelineGenerator;
class RenderingMaterialInstance;
struct ForwardEngineResources;

class InstancedMeshField {
 public:
  // Engine render passes an instanced submesh can be routed to — no
  // game-specific pass kinds. kShadow (Phase 4 of the volumetric-foliage
  // feature) is orthogonal to the other two: a slot's shadow_material (see
  // SlotInfo/SetSubmeshShadow below) is independent of its main `pass`, so
  // one slot can be e.g. kDeferred-lit AND shadow-casting at once.
  enum class PassKind { kDeferred, kForwardOpaque, kShadow };

  // `capacity`/`num_models`/`model_lods` forward straight to
  // GpuInstanceRenderer's ctor (note the reordered `num_submeshes` param,
  // ahead of `model_lods` here, matching this class's declaration order).
  // `model_lods` holds one LOD chain per model -- see
  // GpuInstanceRenderer::ModelLod for the per-model runtime level count.
  InstancedMeshField(wgpu::Device device, wgpu::Queue queue,
                     GpuPipelineGenerator& pipeline_generator,
                     uint32_t capacity, uint32_t num_models,
                     uint32_t num_submeshes,
                     std::span<const GpuInstanceRenderer::ModelLod> model_lods);

  bool IsValid() const { return renderer_.IsValid(); }

  void UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput> instances) {
    renderer_.UploadInstances(instances);
  }

  // Configure one (model, lod, submesh) slot: its mesh (forwarded to
  // renderer_.SetBucketSubmesh(BucketId(model, lod), submesh, ...)), which
  // engine pass draws it, and the material instance to draw it with.
  // `material`'s `bucketId` param must already equal
  // GpuInstanceRenderer::BucketId(model, lod) (the caller's responsibility —
  // this class never sets material parameters). Not owned; `material` (and
  // the vertex/index buffers) must outlive this field's use. `material`
  // may be nullptr to clear/skip the slot.
  //
  // ORDER CONTRACT: this overwrites the slot's WHOLE SlotInfo, including any
  // shadow_material a prior SetSubmeshShadow call attached to it (reset to
  // nullptr) — a slot repurposed via SetSubmesh (new mesh/material) must not
  // keep casting a shadow left over from its previous configuration. Call
  // SetSubmeshShadow AFTER SetSubmesh (every caller in this codebase already
  // does) to (re)attach a shadow material for the slot's new configuration.
  void SetSubmesh(uint32_t model, uint32_t lod, uint32_t submesh,
                  wgpu::Buffer vertex_buffer, wgpu::Buffer index_buffer,
                  wgpu::IndexFormat index_format, uint32_t index_count,
                  PassKind pass, RenderingMaterialInstance* material);

  // Attaches (or clears, via nullptr) a shadow-pass material to an ALREADY
  // configured (model, lod, submesh) slot — geometry comes from whatever
  // SetSubmesh set for that slot (there is no separate shadow mesh; the
  // shadow draw reuses the exact same vertex/index buffers), so call
  // SetSubmesh for the slot first. Not owning, same lifetime contract as
  // SetSubmesh. `material`'s `bucketId` param must equal
  // GpuInstanceRenderer::BucketId(model, lod), same as SetSubmesh's material.
  // A shadow material must not declare @group(2) (no engine resources are
  // bound for Draw(kShadow)) — see Draw()'s doc comment.
  //
  // ORDER CONTRACT: must run AFTER SetSubmesh for this slot — SetSubmesh
  // resets shadow_material to nullptr (see its own doc comment above), so
  // calling this first would just have its attachment wiped by the following
  // SetSubmesh.
  void SetSubmeshShadow(uint32_t model, uint32_t lod, uint32_t submesh,
                        RenderingMaterialInstance* material);

  // True iff any configured slot uses this PassKind. For kDeferred/
  // kForwardOpaque: any slot whose main `pass` matches with a non-null
  // `material`. For kShadow: any slot with a non-null `shadow_material`,
  // REGARDLESS of that slot's main `pass` (shadow-casting is orthogonal to
  // the main pass — see PassKind's doc comment). Lets a driver
  // (SceneRenderer) skip a Cull()/Draw() call entirely when this field has
  // nothing for that pass.
  bool HasPass(PassKind pass) const;

  // Dispatches the GPU cull/LOD/compaction compute passes against the
  // camera's frustum (feeding Draw(..., PassKind::kDeferred/kForwardOpaque)).
  // See GpuInstanceRenderer::Cull's encoder-sequencing contract: must run
  // before any render pass is begun on the same encoder.
  void Cull(FrameContext& frame, const Camera& camera) {
    renderer_.Cull(frame, camera);
  }

  // Same, against the light's frustum (feeding Draw(..., PassKind::kShadow)).
  // See GpuInstanceRenderer::CullShadow's doc comment for why this writes a
  // SEPARATE result set from Cull() above. Same encoder-sequencing contract.
  void CullShadow(FrameContext& frame, const Camera& camera,
                  const glm::mat4& light_view_proj) {
    renderer_.CullShadow(frame, camera, light_view_proj);
  }

  // Draws every configured slot whose PassKind == `pass_kind`. Each slot's
  // material is bound (pipeline + group 0) via Bind(); a forward-opaque
  // material that declares @group(2) (shadow map + IBL) additionally gets
  // that group built (lazily, at most once per Draw() call, shared by every
  // group-2 slot drawn this call — all such slots share one layout) from
  // `engine` and bound at group 2, mirroring render_forward.cpp's
  // BuildForwardOpaqueEngineBindGroup + availability gate exactly. Such a
  // slot is SKIPPED (not drawn) when `engine` is null or its
  // shadow_map/ibl_prefiltered/brdf_lut are unavailable — never drawn with a
  // required group 2 left unbound. `engine` is unused (no gate) for slots
  // whose material doesn't declare group 2.
  //
  // CONTRACT: all group-2-declaring materials drawn by ONE field's Draw()
  // call must use the standard 6-entry engine group-2 layout (shadow map +
  // IBL, see BuildForwardOpaqueEngineBindGroup) — the same assumption the
  // forward passes (render_forward.cpp's RenderForwardMeshes) make for every
  // ForwardOpaqueRenderable entity. The shared bind group above is built once,
  // from the FIRST such slot drawn this call, then reused verbatim for every
  // other group-2 slot — including slots resolved through a DIFFERENT
  // MaterialInstanceFactory / pipeline than the first slot's, as long as
  // that pipeline declares a structurally identical (group-equivalent)
  // group-2 layout; WebGPU only requires layout equivalence at SetBindGroup,
  // not the same pipeline or the same layout object. A slot whose material
  // declares group 2 with a DIFFERENT layout would fail Dawn validation when
  // this shared bind group is bound against its pipeline — such a mix is not
  // supported by one field and is the caller's responsibility to avoid.
  //
  // pass_kind == kShadow is a DIFFERENT selection rule from the other two:
  // every slot with a non-null `shadow_material` is drawn (regardless of that
  // slot's main `pass`), routed through GpuInstanceRenderer::CullSet::kShadow
  // (CullShadow()'s result, not Cull()'s) so a shadow-only cast — a slot
  // shadowing without itself being lit — is possible. `engine` is unused for
  // kShadow: shadow materials never declare @group(2) (their WESL gates it
  // behind `@if(!shadow_pass)`), so the group-2 machinery above is inert for
  // them.
  void Draw(RenderPassContext& pass, FrameContext& frame, PassKind pass_kind,
            const ForwardEngineResources* engine = nullptr);

 private:
  GpuInstanceRenderer renderer_;

  struct SlotInfo {
    PassKind pass = PassKind::kDeferred;
    RenderingMaterialInstance* material = nullptr;
    // Independent of `pass`/`material` above — see PassKind's doc comment.
    RenderingMaterialInstance* shadow_material = nullptr;
  };
  std::vector<SlotInfo> slots_;  // size GetNumBuckets() * GetNumSubmeshes()
};

}  // namespace badlands
