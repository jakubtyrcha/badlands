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

}  // namespace

glm::mat4 ForestRenderer::InstanceTransform(
    const foliage::FoliageInstance& inst) const {
  const float s = tree_field_->native_to_world_scale[inst.model] * inst.scale;
  const float base_y = tree_field_->model_bounds[inst.model].bark_local_bounds.min.y;
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

  std::vector<TreeFieldModel> models(catalog_.models.size());
  ParallelFor(catalog_.models.size(), [&](size_t i) {
    models[i] = BuildTreeFieldModel(catalog_.models[i].options,
                                    catalog_.models[i].target_height_m);
  });

  tree_field_ =
      BuildTreeField(device, queue, pipeline_gen, models,
                     static_cast<uint32_t>(stats_.total_instances));
  if (!tree_field_) {
    spdlog::error("ForestRenderer: BuildTreeField failed for {} models",
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
