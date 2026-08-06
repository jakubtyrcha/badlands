#include "game/visual/instanced_lod_model.hpp"

#include <algorithm>

// spdlog's bundled fmt, not a standalone one -- badlands has no separate fmt
// dependency (see the porting note in engine/rendering/shader/shader_reflection.cpp).
#include <spdlog/fmt/fmt.h>

#include "engine/rendering/gpu_instance_renderer.hpp"

namespace badlands {

LodModelBounds ComputeLodModelBounds(const InstancedLodModel& model) {
  LodModelBounds bounds;
  bounds.submesh_bounds.assign(model.submesh_count(), Aabb::Empty());

  for (const std::vector<TexturedMeshResult>& level : model.levels) {
    for (size_t s = 0; s < level.size() && s < bounds.submesh_bounds.size();
         ++s) {
      // Unioning an empty level's bounds unguarded is safe: Aabb::Empty()'s
      // sentinel (min=+FLT_MAX, max=-FLT_MAX) makes Union a no-op, which
      // matters because a legitimately empty submesh at one LOD must not widen
      // or reset the bounds the others contributed.
      bounds.submesh_bounds[s] =
          bounds.submesh_bounds[s].Union(level[s].local_bounds);
    }
  }
  return bounds;
}

std::string ValidateLodModel(const InstancedLodModel& model) {
  const size_t lod_count = model.lod_count();
  if (lod_count == 0) return "has no LOD levels";
  if (lod_count > GpuInstanceRenderer::kMaxLods) {
    return fmt::format("has {} LOD levels, past the engine's kMaxLods cap of {}",
                       lod_count, GpuInstanceRenderer::kMaxLods);
  }
  if (model.submesh_count() == 0) return "has no submesh materials";

  // One ascending cutoff between each adjacent pair. The impostor's cutoff is
  // NOT counted here -- the builder appends that itself (see the header), so a
  // model that supplied one would end up with a chain one level too long.
  if (model.thresholds.size() != lod_count - 1) {
    return fmt::format(
        "has {} thresholds but {} levels (need exactly one cutoff between each "
        "adjacent pair, i.e. {})",
        model.thresholds.size(), lod_count, lod_count - 1);
  }
  // Seeded at 0 and starting at index 0, matching GpuInstanceRenderer exactly
  // (see its ctor): it requires strictly ascending AND POSITIVE, so a chain
  // whose first cutoff is 0 or negative is malformed too. Starting at 1 let
  // that through, and the renderer only LOGS it -- the failure then surfaces as
  // levels that never draw rather than as a build error.
  float prev = 0.0f;
  for (size_t i = 0; i < model.thresholds.size(); ++i) {
    if (!(model.thresholds[i] > prev)) {
      return fmt::format(
          "has a non-ascending or non-positive threshold at index {} ({} does "
          "not exceed {}) -- the level below it can never be selected",
          i, model.thresholds[i], prev);
    }
    prev = model.thresholds[i];
  }

  for (size_t lod = 0; lod < lod_count; ++lod) {
    if (model.levels[lod].size() > model.submesh_count()) {
      return fmt::format(
          "LOD {} holds {} submeshes but only {} materials were supplied",
          lod, model.levels[lod].size(), model.submesh_count());
    }
  }

  for (size_t s = 0; s < model.submesh_count(); ++s) {
    const InstancedMaterialSpec& mat = model.submesh_materials[s];
    if (mat.shader_path.empty()) {
      return fmt::format("submesh {} has no shader_path", s);
    }
    // bucketId is per (model, lod) and only the builder knows it; a supplied
    // one would be silently overwritten, so it is rejected instead.
    if (mat.uniforms.contains("bucketId")) {
      return fmt::format(
          "submesh {} sets \"bucketId\", which the field builder owns", s);
    }
  }

  for (const ImpostorBakeSubmesh& sub : model.impostor.submeshes) {
    if (sub.lod >= lod_count) {
      return fmt::format("impostor bakes from LOD {}, past the model's {}",
                         sub.lod, lod_count);
    }
    if (sub.submesh >= model.levels[sub.lod].size()) {
      return fmt::format(
          "impostor bakes submesh {} of LOD {}, which holds only {}",
          sub.submesh, sub.lod, model.levels[sub.lod].size());
    }
  }

  return {};
}

}  // namespace badlands
