#include "engine/rendering/instanced_mesh_field.hpp"

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
  // SetBucketSubmesh itself validates (bucket, submesh) against the
  // renderer's dimensions and logs+no-ops if out of range.
  renderer_.SetBucketSubmesh(bucket, submesh, vertex_buffer, index_buffer,
                             index_format, index_count);
  if (bucket >= renderer_.GetNumBuckets() ||
      submesh >= renderer_.GetNumSubmeshes()) {
    return;
  }
  slots_[bucket * renderer_.GetNumSubmeshes() + submesh] =
      SlotInfo{pass, material};
}

bool InstancedMeshField::HasPass(PassKind pass) const {
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
  // Mirrors render_forward.cpp's RenderForwardMeshes gate exactly: a group-2
  // material is only drawn when all three resources it needs are present.
  const bool group2_available = engine != nullptr &&
                                static_cast<bool>(engine->shadow_map) &&
                                static_cast<bool>(engine->ibl_prefiltered) &&
                                static_cast<bool>(engine->brdf_lut);
  // Built lazily from the first group-2 slot drawn this call, then reused —
  // every group-2 slot in one Draw() shares the same 6-entry layout (same
  // pattern as render_forward.cpp's per-pass `engine_bg`).
  wgpu::BindGroup engine_bg;

  const uint32_t num_submeshes = renderer_.GetNumSubmeshes();
  renderer_.Draw(
      pass, frame,
      [&](uint32_t bucket, uint32_t submesh) -> RenderingMaterialInstance* {
        const uint32_t slot = bucket * num_submeshes + submesh;
        if (slot >= slots_.size()) {
          return nullptr;
        }
        const SlotInfo& info = slots_[slot];
        if (info.material == nullptr || info.pass != pass_kind) {
          return nullptr;  // not configured, or belongs to the other pass
        }

        const bool declares_g2 = info.material->DeclaresBindGroup(2);
        if (declares_g2 && !group2_available) {
          // Never draw with a required group 2 left unbound.
          return nullptr;
        }
        if (!info.material->Bind(pass, frame)) {
          return nullptr;
        }
        if (declares_g2) {
          if (!engine_bg) {
            engine_bg = BuildForwardOpaqueEngineBindGroup(info.material, frame,
                                                          *engine);
          }
          pass.SetBindGroup(2, engine_bg);
        }
        return info.material;
      });
}

}  // namespace badlands
