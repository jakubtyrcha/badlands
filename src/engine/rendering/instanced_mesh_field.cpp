#include "engine/rendering/instanced_mesh_field.hpp"

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "engine/rendering/passes/render_forward.hpp"

namespace badlands {

InstancedMeshField::InstancedMeshField(
    wgpu::Device device, wgpu::Queue queue,
    GpuPipelineGenerator& pipeline_generator, uint32_t capacity,
    uint32_t num_models, uint32_t num_submeshes,
    std::array<float, GpuInstanceRenderer::kMaxLods - 1> lod_thresholds)
    : renderer_(device, queue, pipeline_generator, capacity, num_models,
                lod_thresholds, num_submeshes) {
  slots_.resize(uint64_t{renderer_.GetNumBuckets()} *
                renderer_.GetNumSubmeshes());
}

void InstancedMeshField::SetSubmesh(uint32_t model, uint32_t lod,
                                    uint32_t submesh,
                                    wgpu::Buffer vertex_buffer,
                                    wgpu::Buffer index_buffer,
                                    wgpu::IndexFormat index_format,
                                    uint32_t index_count, PassKind pass,
                                    RenderingMaterialInstance* material) {
  const uint32_t bucket = GpuInstanceRenderer::BucketId(model, lod);
  // One up-front range check covers both renderer_.SetBucketSubmesh (which
  // would otherwise separately log+no-op the same out-of-range case) and
  // slots_'s own indexing — no post-call re-check needed.
  if (bucket >= renderer_.GetNumBuckets() ||
      submesh >= renderer_.GetNumSubmeshes()) {
    spdlog::error(
        "InstancedMeshField::SetSubmesh: (model={}, lod={}) -> bucket={} or "
        "submesh={} out of range (num_buckets={}, num_submeshes={})",
        model, lod, bucket, submesh, renderer_.GetNumBuckets(),
        renderer_.GetNumSubmeshes());
    return;
  }
  renderer_.SetBucketSubmesh(bucket, submesh, vertex_buffer, index_buffer,
                             index_format, index_count);
  // Full-reset contract: SetSubmesh overwrites the WHOLE SlotInfo, including
  // shadow_material -- a slot repurposed via a second SetSubmesh call (new
  // mesh/material) must not keep casting a shadow from whatever material an
  // earlier, now-unrelated SetSubmeshShadow call attached. Callers that want
  // the slot to keep casting a shadow call SetSubmeshShadow again AFTER this
  // (see both methods' header comments for the order contract).
  slots_[bucket * renderer_.GetNumSubmeshes() + submesh] =
      SlotInfo{.pass = pass, .material = material};
}

void InstancedMeshField::SetSubmeshShadow(uint32_t model, uint32_t lod,
                                          uint32_t submesh,
                                          RenderingMaterialInstance* material) {
  const uint32_t bucket = GpuInstanceRenderer::BucketId(model, lod);
  const uint32_t num_submeshes = renderer_.GetNumSubmeshes();
  if (bucket >= renderer_.GetNumBuckets() || submesh >= num_submeshes) {
    spdlog::error(
        "InstancedMeshField::SetSubmeshShadow: (model={}, lod={}) -> "
        "bucket={} or submesh={} out of range (num_buckets={}, "
        "num_submeshes={})",
        model, lod, bucket, submesh, renderer_.GetNumBuckets(), num_submeshes);
    return;
  }
  slots_[bucket * num_submeshes + submesh].shadow_material = material;
  // Registering a real shadow material provisions the renderer's lazily-
  // created shadow cull buffer set/bind groups (see
  // GpuInstanceRenderer::EnsureShadowCull()) ahead of the first CullShadow()
  // call, rather than waiting for a driver (e.g. SceneRenderer) to notice
  // HasPass(kShadow) and call CullShadow() itself -- a no-op if that's
  // already happened, or if `material` is nullptr (clearing a slot, which
  // doesn't need shadow resources to exist).
  if (material != nullptr) {
    renderer_.EnsureShadowCull();
  }
}

bool InstancedMeshField::HasPass(PassKind pass) const {
  if (pass == PassKind::kShadow) {
    for (const SlotInfo& slot : slots_) {
      if (slot.shadow_material != nullptr) {
        return true;
      }
    }
    return false;
  }
  for (const SlotInfo& slot : slots_) {
    if (slot.material != nullptr && slot.pass == pass) {
      return true;
    }
  }
  return false;
}

void InstancedMeshField::Draw(RenderPassContext& pass, FrameContext& frame,
                              PassKind pass_kind,
                              const ForwardEngineResources* engine) {
  // Shares render_forward.cpp's RenderForwardMeshes gate exactly (same
  // ForwardOpaqueEngineAvailable helper): a group-2 material is only drawn
  // when all three resources it needs are present. Irrelevant for kShadow
  // (shadow materials never declare group 2 — group2_available stays unused
  // by them either way).
  const bool group2_available =
      engine != nullptr && ForwardOpaqueEngineAvailable(*engine);
  // Built lazily from the first group-2 slot drawn this call, then reused —
  // every group-2 slot in one Draw() shares the same 6-entry layout (same
  // pattern as render_forward.cpp's per-pass `engine_bg`).
  wgpu::BindGroup engine_bg;

  const bool is_shadow = pass_kind == PassKind::kShadow;
  const uint32_t num_submeshes = renderer_.GetNumSubmeshes();
  renderer_.Draw(
      pass, frame,
      [&](uint32_t bucket, uint32_t submesh) -> RenderingMaterialInstance* {
        const uint32_t slot = bucket * num_submeshes + submesh;
        if (slot >= slots_.size()) {
          return nullptr;
        }
        const SlotInfo& info = slots_[slot];
        // kShadow draws every slot with a shadow_material, regardless of the
        // slot's main `pass` (shadow-casting is orthogonal — see PassKind's
        // doc comment); the other pass kinds keep the exact-match filter.
        RenderingMaterialInstance* material =
            is_shadow ? info.shadow_material
                      : (info.pass == pass_kind ? info.material : nullptr);
        if (material == nullptr) {
          return nullptr;  // not configured, or belongs to a different pass
        }

        const bool declares_g2 = material->DeclaresBindGroup(2);
        if (declares_g2 && !group2_available) {
          // Never draw with a required group 2 left unbound.
          return nullptr;
        }
        if (!material->Bind(pass, frame)) {
          return nullptr;
        }
        if (declares_g2) {
          if (!engine_bg) {
            engine_bg =
                BuildForwardOpaqueEngineBindGroup(material, frame, *engine);
          }
          pass.SetBindGroup(2, engine_bg);
        }
        return material;
      },
      is_shadow ? GpuInstanceRenderer::CullSet::kShadow
                : GpuInstanceRenderer::CullSet::kMain);
}

}  // namespace badlands
