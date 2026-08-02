#pragma once

// Draws a placed FoliageField: owns the multi-model TreeField, does the CPU
// coarse cull over 32 m foliage cells, and keeps the GPU instance set in sync.
//
// TWO LEVELS OF CULLING, deliberately. The GPU already culls and LOD-selects
// every instance (GpuInstanceRenderer), so the CPU pass here is not about
// visibility accuracy -- it is about how much data has to reach the GPU at all.
// One frustum test per 32 m cell stands in for hundreds of per-instance tests
// and, more importantly, decides what gets uploaded.
//
// UPLOAD ON CHANGE, not per frame. Re-packing and re-uploading every visible
// instance each frame would be pure traffic for no benefit: cells are 32 m, so
// the visible SET changes rarely even while the camera moves continuously. The
// instance buffer is rebuilt only when that set actually changes.
//
// This is a game world component, not a mapview feature -- mapview is simply
// its first consumer.

#include <cstdint>
#include <memory>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/core/camera.hpp"
#include "engine/rendering/gpu_instance_renderer.hpp"
#include "engine/rendering/instanced_mesh_field.hpp"
#include "foliage/foliage_types.hpp"
#include "game/visual/forest_catalog.hpp"
#include "game/visual/impostor_baker.hpp"
#include "game/visual/tree_field.hpp"

namespace badlands {

class GpuPipelineGenerator;

struct ForestRendererStats {
  size_t total_instances = 0;
  size_t visible_instances = 0;
  int total_cells = 0;
  int visible_cells = 0;
  // How many times the instance buffer has been rebuilt since Build(). Cheap
  // evidence that the upload-on-change rule is actually holding.
  int uploads = 0;
};

// Builds every model's CPU geometry (in parallel -- the meshing is pure CPU and
// independent per model) and, as it goes, MEASURES each model's crown radius and
// writes it into `catalog.type.models[i].radius_m`.
//
// This must run BEFORE GenerateFoliage, and that ordering is the whole reason
// the mesh build is a separate step rather than something ForestRenderer::Build
// does internally. The sampler spaces instances by a circle around each model,
// and the only honest source for that circle is the geometry the model actually
// draws: a radius typed into the forest file is a guess that goes stale the
// moment a preset is retuned, and a stale one shows up either as crowns growing
// through each other or as a forest inexplicably too thin.
//
// The measurement is the model's bark unioned with its FINEST non-empty crown
// LOD, at scale 1 (the sampler applies each instance's own scale). Deliberately
// not the union over every LOD, which is what TreeField::model_bounds holds:
// coarse voxel crowns overscale past the cards they came from, so that union
// runs 25-40% wide as a silhouette and spacing by it opens the stand to 1.8x
// the ground area per tree for geometry no one can see. The all-LOD union is
// still the right answer for the cell-cull padding, which asks a different
// question -- see ForestRenderer::Build. crown_bounds.hpp covers why a radius
// is a half-extent and not a box diagonal.
//
// Returns the prepared models to hand straight to ForestRenderer::Build, so
// nothing is generated twice.
std::vector<TreeFieldModel> BuildForestModels(ForestCatalog& catalog);

class ForestRenderer {
 public:
  // Uploads already-prepared models (BuildForestModels above) into one
  // InstancedMeshField. `catalog` and `field` are taken by value and kept: the
  // renderer needs the model table for transforms and the cell grid for
  // culling, for its lifetime.
  //
  // `models` must be what BuildForestModels returned FOR THIS catalog: the
  // instances in `field` carry model indices into the catalog, while the
  // uploaded per-model tables are sized by `models`, so a mismatched pair is
  // an out-of-bounds read on every frame rather than a failure here. Build
  // rejects a mismatched size, and a catalog whose radii were never measured.
  //
  // Returns false (after logging) on either of those, or if the field could
  // not be built. An EMPTY FoliageField is not a failure -- it builds a valid,
  // empty renderer, which is exactly what a map with no forest biome should
  // produce, and it costs no GPU resources.
  bool Build(wgpu::Device device, wgpu::Queue queue,
             GpuPipelineGenerator& pipeline_gen, ForestCatalog catalog,
             std::vector<TreeFieldModel> models, foliage::FoliageField field);

  // Culls cells and re-uploads only if the visible set changed. Cheap to call
  // every frame.
  //
  // `sun_direction` points TOWARD the sun (SceneContext::sun_direction's
  // convention) and is load-bearing, not cosmetic: a cell is kept if it is
  // visible OR if its SHADOW could fall into view. The uploaded instance set
  // feeds both of GpuInstanceRenderer's cull sets, so an instance dropped here
  // cannot be recovered by the engine's separate light-frustum cull -- culling
  // on the camera alone would delete shadow casters, and stands just off-screen
  // would drop their shadows off visible ground.
  void Update(const Camera& camera, const glm::vec3& sun_direction);

  // Null until a successful Build, and null for an empty forest -- callers
  // should skip wiring it into SceneContext in that case.
  InstancedMeshField* instanced_field() const {
    return tree_field_ ? tree_field_->field.get() : nullptr;
  }

  bool empty() const { return stats_.total_instances == 0; }
  const ForestRendererStats& stats() const { return stats_; }

 private:
  // World transform for one placed instance, combining the model's native->world
  // scale, the instance's own scale jitter, its yaw, and the rest-on-ground
  // offset that puts the trunk base at the instance position.
  glm::mat4 InstanceTransform(const foliage::FoliageInstance& inst) const;

  std::unique_ptr<TreeField> tree_field_;
  // The LOD4 impostor atlas, baked in Build. Held here because TreeField's
  // materials bind its views and only reference them.
  ImpostorBakeResult impostor_;
  ForestCatalog catalog_;
  foliage::FoliageField field_;

  std::vector<GpuInstanceRenderer::InstanceInput> instances_;
  // Per-cell visibility from the last Update, so a changed set is detectable
  // without re-packing the instances to compare them.
  std::vector<uint8_t> visible_;
  std::vector<uint8_t> next_visible_;
  bool uploaded_once_ = false;

  // Largest world-space horizontal crown reach across every model, at that
  // model's largest instance scale. A cell's XZ box is padded by it: a 22 m oak
  // planted a metre inside a cell edge has a crown metres wider than the cell,
  // so the bare 32 m footprint would cull trees that are still partly on
  // screen, popping them in at the frame edge.
  float max_crown_radius_m_ = 0.0f;
  // Lowest ground any instance stands on, for bounding how far a shadow can
  // travel before it lands.
  float min_ground_y_ = 0.0f;

  ForestRendererStats stats_;
};

}  // namespace badlands
