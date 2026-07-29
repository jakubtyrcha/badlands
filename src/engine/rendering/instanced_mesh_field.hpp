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
//   forward-opaque pass.
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
  // Engine render passes an instanced submesh can be routed to. Deliberately
  // just the two passes this field currently drives (see the design doc's
  // "out of scope" section for shadow casting) — no game-specific pass kinds.
  enum class PassKind { kDeferred, kForwardOpaque };

  // `capacity`/`num_models`/`lod_thresholds` forward straight to
  // GpuInstanceRenderer's ctor (note the reordered `num_submeshes` param,
  // ahead of `lod_thresholds` here, matching this class's declaration order).
  InstancedMeshField(wgpu::Device device, wgpu::Queue queue,
                     GpuPipelineGenerator& pipeline_generator,
                     uint32_t capacity, uint32_t num_models,
                     uint32_t num_submeshes,
                     std::array<float, GpuInstanceRenderer::kMaxLods - 1>
                         lod_thresholds);

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
  void SetSubmesh(uint32_t model, uint32_t lod, uint32_t submesh,
                  wgpu::Buffer vertex_buffer, wgpu::Buffer index_buffer,
                  wgpu::IndexFormat index_format, uint32_t index_count,
                  PassKind pass, RenderingMaterialInstance* material);

  // True iff any configured slot uses this PassKind (with a non-null
  // material). Lets a driver (SceneRenderer) skip a Draw() call entirely when
  // this field has nothing for that pass.
  bool HasPass(PassKind pass) const;

  // Dispatches the GPU cull/LOD/compaction compute passes. See
  // GpuInstanceRenderer::Cull's encoder-sequencing contract: must run before
  // any render pass is begun on the same encoder.
  void Cull(FrameContext& frame, const Camera& camera) {
    renderer_.Cull(frame, camera);
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
  void Draw(RenderPassContext& pass, FrameContext& frame, PassKind pass_kind,
            const ForwardEngineResources* engine = nullptr);

 private:
  GpuInstanceRenderer renderer_;

  struct SlotInfo {
    PassKind pass = PassKind::kDeferred;
    RenderingMaterialInstance* material = nullptr;
  };
  std::vector<SlotInfo> slots_;  // size GetNumBuckets() * GetNumSubmeshes()
};

}  // namespace badlands
