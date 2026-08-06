#include "game/visual/forest_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/frustum.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "game/visual/crown_bounds.hpp"
#include "game/visual/foliage_cell_cull.hpp"

namespace badlands {

namespace {

// Runs `body(i)` for i in [0, n) across hardware threads. The per-model mesh
// build is pure CPU, independent, and the dominant cost of loading a forest
// (28 skeletons + 112 voxelizations), so it is worth not doing serially.
template <typename F>
void ParallelFor(size_t n, F&& body) {
  if (n == 0) return;
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const size_t workers = std::min<size_t>(hw, n);
  if (workers <= 1) {
    for (size_t i = 0; i < n; ++i) body(i);
    return;
  }

  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (size_t w = 0; w < workers; ++w) {
    pool.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= n) break;
        body(i);
      }
    });
  }
  for (std::thread& t : pool) t.join();
}

// What the model looks like CLOSE UP: its bark, unioned with its LOD0 crown
// only.
//
// Deliberately not LodModelBounds::Combined(), which unions every crown LOD.
// That union is the right answer for the GPU bounds sphere -- a coarse LOD's
// voxels overscale past the cards they came from, and an instance's sphere has
// to cover whichever LOD is actually drawn. It is the wrong answer for spacing,
// where it silently pads every tree with voxel quantization it does not appear
// to have: measured on the shipped forest, the union runs 25-40% wider than
// LOD0 (Oak (large) v0: 13.9 m against 10.1 m), and spacing by it opens the
// stand to 1.8x the ground area per tree for geometry no one can see.
// A crown LOD may legitimately voxelize to nothing (see leaf_voxelizer.hpp),
// so take the FINEST level that actually produced geometry rather than index 0
// blindly. Falling through to bark-only would report a radius of roughly the
// trunk's half-width, and that species would then pack at trunk distance with
// crowns straight through each other -- with nothing in the log but a small,
// entirely plausible-looking number.
Aabb SilhouetteBounds(const InstancedLodModel& model) {
  if (model.levels.empty()) return Aabb::Empty();
  // Submesh 0 is the tree's bark at every level; submesh 1 is its crown. This
  // is the one place the forest still reads the tree producer's submesh layout
  // directly, and it is why kTreeBarkSubmesh/kTreeCrownSubmesh are named in
  // tree_lod_model.hpp rather than being bare literals.
  Aabb bounds = model.levels[0][kTreeBarkSubmesh].local_bounds;
  for (const std::vector<TexturedMeshResult>& level : model.levels) {
    if (level.size() <= kTreeCrownSubmesh) continue;
    const TexturedMeshResult& crown = level[kTreeCrownSubmesh];
    if (crown.mesh.vertex_count == 0) continue;
    bounds = bounds.Union(crown.local_bounds);
    break;
  }
  return bounds;
}

}  // namespace

std::vector<InstancedLodModel> BuildForestModels(ForestCatalog& catalog) {
  std::vector<InstancedLodModel> models(catalog.models.size());
  ParallelFor(catalog.models.size(), [&](size_t i) {
    models[i] = BuildTreeFieldModel(catalog.models[i].options,
                                    catalog.models[i].target_height_m);
  });

  for (size_t i = 0; i < models.size(); ++i) {
    catalog.type.models[i].radius_m =
        CrownRadiusM(SilhouetteBounds(models[i]), models[i].native_to_world_scale);
  }

  // Per-layer crown radius range, named. This is the number a person editing
  // the forest file cannot otherwise see, and it sets that layer's density
  // ceiling outright: under a sum-of-radii rule two neighbours of radius r
  // stand 2r apart, so a layer's spacing is legible from this line alone and a
  // grid_m below it is simply wasted candidates.
  for (size_t li = 0; li < catalog.type.layers.size(); ++li) {
    const foliage::FoliageLayer& layer = catalog.type.layers[li];
    if (layer.model_count == 0) continue;

    float lo = std::numeric_limits<float>::max(), hi = 0.0f;
    size_t widest = layer.first_model;
    for (uint16_t k = 0; k < layer.model_count; ++k) {
      const size_t mi = static_cast<size_t>(layer.first_model) + k;
      const float r = catalog.type.models[mi].radius_m;
      lo = std::min(lo, r);
      if (r > hi) { hi = r; widest = mi; }
    }
    spdlog::info(
        "forest models: layer {} crown radius {:.1f}-{:.1f} m (widest: {}), "
        "so neighbours stand {:.1f}-{:.1f} m apart",
        li, lo, hi, catalog.models[widest].debug_name, 2.0f * lo, 2.0f * hi);
  }
  return models;
}

glm::mat4 ForestRenderer::InstanceTransform(
    const foliage::FoliageInstance& inst) const {
  const float s = tree_field_->native_to_world_scale[inst.model] * inst.scale;
  const float base_y =
      tree_field_->model_bounds[inst.model].submesh_bounds[kTreeBarkSubmesh].min.y;
  // Rest-on-ground: the generated tree's base sits at local base_y, so lift by
  // -base_y * s to put it exactly on the instance position. Same derivation the
  // model viewer uses for its preview transform.
  return glm::translate(glm::mat4(1.0f), inst.position) *
         glm::rotate(glm::mat4(1.0f), inst.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
         glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -base_y * s, 0.0f)) *
         glm::scale(glm::mat4(1.0f), glm::vec3(s));
}

bool ForestRenderer::Build(wgpu::Device device, wgpu::Queue queue,
                           GpuPipelineGenerator& pipeline_gen,
                           ForestCatalog catalog,
                           std::vector<InstancedLodModel> models,
                           foliage::FoliageField field) {
  catalog_ = std::move(catalog);
  field_ = std::move(field);

  stats_ = {};
  stats_.total_instances = field_.InstanceCount();
  stats_.total_cells = field_.cells_x * field_.cells_z;

  if (catalog_.empty() || stats_.total_instances == 0) {
    // Not a failure: a map whose forest biome is absent legitimately places
    // nothing, and that should cost no GPU resources at all.
    spdlog::info("ForestRenderer: nothing to plant ({} models, {} instances)",
                 catalog_.models.size(), stats_.total_instances);
    return true;
  }

  // The mesh build and the upload are two calls now, and a caller has to pair
  // them. An unpaired one is not a crash here but a silent out-of-bounds read
  // every frame: instances carry model indices into the CATALOG, while
  // native_to_world_scale and model_bounds are sized by `models`.
  if (models.size() != catalog_.models.size()) {
    spdlog::error(
        "ForestRenderer: {} prepared models against a {}-model catalog -- pass "
        "the vector BuildForestModels returned for THIS catalog",
        models.size(), catalog_.models.size());
    return false;
  }
  // And a catalog whose radii were never measured would have been placed with
  // no spacing at all. GenerateFoliage refuses that outright, so reaching here
  // with an all-zero table means the field came from somewhere else entirely.
  const bool any_radius =
      std::any_of(catalog_.type.models.begin(), catalog_.type.models.end(),
                  [](const foliage::FoliageModel& m) { return m.radius_m > 0.0f; });
  if (!any_radius) {
    spdlog::error(
        "ForestRenderer: every model has radius 0 -- this catalog never went "
        "through BuildForestModels, so nothing was spaced");
    return false;
  }

  // Bake the LOD4 impostor atlas from the same models the voxel chain uses.
  // A failed bake is NOT fatal: the forest simply keeps the voxel-only chain,
  // which is the behaviour that existed before LOD4 and is far better than
  // refusing to render a forest at all.
  impostor_ = BakeImpostorAtlas(device, queue, pipeline_gen, models);
  InstancedLodImpostor impostor_slot;
  if (impostor_.ok) {
    impostor_slot.atlas = &impostor_.atlas;
    impostor_slot.placement = impostor_.placement;
  } else {
    spdlog::warn(
        "ForestRenderer: impostor bake failed -- falling back to the "
        "voxel-only LOD chain");
  }

  tree_field_ = BuildInstancedLodField(
      device, queue, pipeline_gen, models,
      static_cast<uint32_t>(stats_.total_instances), impostor_slot);
  if (!tree_field_) {
    spdlog::error("ForestRenderer: BuildInstancedLodField failed for {} models",
                  models.size());
    return false;
  }

  instances_.reserve(stats_.total_instances);
  visible_.assign(static_cast<size_t>(stats_.total_cells), 0);
  next_visible_.assign(static_cast<size_t>(stats_.total_cells), 0);

  // How far a crown can reach horizontally past the trunk it stands on, at the
  // largest scale any instance of that model can take. One global maximum
  // rather than a per-cell figure: it is a single float, it is conservative,
  // and over-including a cell only costs a GPU per-instance cull that runs
  // anyway.
  //
  // NOT the sampler's radius, and the difference is load-bearing. Spacing asks
  // "how wide does this tree LOOK", so it measures LOD0 (see SilhouetteBounds).
  // This asks "how far can anything DRAWN reach out of the cell", and at
  // distance what is drawn is a coarse voxel crown that overscales past its
  // source cards -- 25-40% wider on the shipped forest. Padding by the LOD0
  // figure culls cells whose coarse crowns are still on screen, which is the
  // frame-edge pop-in the padding exists to prevent, and it only ever shows up
  // at LOD range. So this reads the union over every LOD.
  max_crown_radius_m_ = 0.0f;
  for (size_t m = 0; m < tree_field_->model_count(); ++m) {
    const Aabb local = tree_field_->model_bounds[m].Combined();
    const float half_extent =
        std::max({std::abs(local.min.x), std::abs(local.max.x),
                  std::abs(local.min.z), std::abs(local.max.z)});
    const float scale = tree_field_->native_to_world_scale[m] *
                        catalog_.type.models[m].scale_range.y;
    max_crown_radius_m_ = std::max(max_crown_radius_m_, half_extent * scale);
  }

  min_ground_y_ = std::numeric_limits<float>::max();
  for (const foliage::CellYBounds& yb : field_.cell_y) {
    if (!yb.empty()) min_ground_y_ = std::min(min_ground_y_, yb.min_y);
  }
  if (min_ground_y_ == std::numeric_limits<float>::max()) min_ground_y_ = 0.0f;

  // Per-layer counts, because the total alone cannot tell a forest that is too
  // dense from one whose undergrowth is too dense -- and those are different
  // knobs (each layer's own grid_m).
  std::vector<int> per_layer(catalog_.type.layers.size(), 0);
  for (const std::vector<foliage::FoliageInstance>& cell : field_.cells) {
    for (const foliage::FoliageInstance& inst : cell) {
      if (inst.layer < per_layer.size()) per_layer[inst.layer]++;
    }
  }
  std::string breakdown;
  for (size_t i = 0; i < per_layer.size(); ++i) {
    breakdown += (i ? ", " : "");
    breakdown += "L" + std::to_string(i) + "=" + std::to_string(per_layer[i]);
  }
  spdlog::info(
      "ForestRenderer: {} instances ({}) across {} cells, {} models",
      stats_.total_instances, breakdown, stats_.total_cells, models.size());
  return true;
}

void ForestRenderer::Update(const Camera& camera,
                            const glm::vec3& sun_direction) {
  if (!tree_field_ || stats_.total_instances == 0) return;

  const Frustum frustum = Frustum::FromViewProj(camera.GetProj() * camera.GetView());

  std::fill(next_visible_.begin(), next_visible_.end(), 0);
  int visible_cells = 0;
  for (int cz = 0; cz < field_.cells_z; ++cz) {
    for (int cx = 0; cx < field_.cells_x; ++cx) {
      const size_t ci = static_cast<size_t>(field_.CellIndex(cx, cz));
      const foliage::CellYBounds& yb = field_.cell_y[ci];
      if (yb.empty()) continue;  // no instances -- nothing to test or draw

      const Aabb cell_box = FoliageCellBounds(
          field_.CellOrigin(cx, cz), foliage::kFoliageCellSizeM, yb.min_y,
          yb.max_y, max_crown_radius_m_);
      if (!FoliageCellVisibleOrCasts(frustum, cell_box, sun_direction,
                                     min_ground_y_)) {
        continue;
      }

      next_visible_[ci] = 1;
      visible_cells++;
    }
  }

  // Upload only when the visible SET changed. Cells are 32 m, so this is rare
  // even under continuous camera motion -- which is the entire point of filing
  // instances into cells in the first place.
  if (uploaded_once_ && next_visible_ == visible_) return;

  visible_.swap(next_visible_);
  stats_.visible_cells = visible_cells;

  instances_.clear();
  for (size_t ci = 0; ci < visible_.size(); ++ci) {
    if (!visible_[ci]) continue;
    for (const foliage::FoliageInstance& inst : field_.cells[ci]) {
      const glm::mat4 xf = InstanceTransform(inst);
      const Aabb world =
          tree_field_->model_bounds[inst.model].Combined().TransformedBy(xf);
      const glm::vec3 center = world.Center();

      GpuInstanceRenderer::InstanceInput in;
      in.transform = xf;
      in.bounds_sphere = glm::vec4(
          center, std::max(glm::length(world.max - center), 0.01f));
      in.model_info = glm::uvec4(inst.model, 0u, 0u, 0u);
      instances_.push_back(in);
    }
  }

  stats_.visible_instances = instances_.size();
  stats_.uploads++;
  uploaded_once_ = true;
  tree_field_->field->UploadInstances(instances_);
}

}  // namespace badlands
