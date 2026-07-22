#include "game/map/cluster_terrain.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <string>

#include <glm/gtc/matrix_inverse.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/app/render_context.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/frustum.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"

namespace badlands {

ClusterTerrain::ClusterTerrain() = default;
ClusterTerrain::~ClusterTerrain() = default;

bool ClusterTerrain::Build(const MapData& map, const RenderContext& ctx,
                           entt::registry& registry, const glm::mat4& model,
                           const TerrainClusterParams& params) {
  registry_ = &registry;
  model_ = model;
  inv_model_ = glm::inverse(model);

  // Build the cluster-LOD DAG from the frozen MapData lattice.
  dag_ = BuildTerrainClusterDag(map, params);

  // Deferred vertex-color cluster material (game-owned; no engine-side
  // MaterialLibrary entry). Mirrors the engine's terrain descriptor but for
  // kTerrainCluster geometry + the flat-color fs_gbuffer entry, and needs no
  // textures -- the per-vertex biome color IS the albedo.
  FactoryDescriptor desc;
  desc.shader_name = "terrain_cluster";
  desc.shader_path = "material/terrain_cluster.wesl";
  desc.fs_entry = "fs_gbuffer";
  desc.supported_pass_types = {MaterialPassType::kDeferred};
  desc.supported_geometry_types = {GeometryType::kTerrainCluster};
  desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                        GBuffer::kMaterialFormat};
  desc.depth_format = GBuffer::kDepthFormat;
  factory_ =
      BuildMaterialInstanceFactory(desc, ctx.device, ctx.queue, ctx.pipeline_gen);
  if (!factory_) {
    spdlog::error("ClusterTerrain: failed to build terrain_cluster material factory");
    return false;
  }

  // One entity holds the whole shared cluster mesh; MeshDrawRangesComponent
  // draws each selected cluster as its own culled DrawIndexed range. The DAG's
  // geometry is MOVED into the mesh: the build runs once and SelectClusters reads
  // only clusters/groups, never vertices/indices, so the DAG keeps no copy.
  const entt::entity e = registry.create();
  auto& mesh = registry.emplace<StaticTexturedMeshComponent>(e);
  mesh.vertices = std::move(dag_.vertices);
  mesh.indices = std::move(dag_.indices);
  mesh.vertex_count =
      static_cast<uint32_t>(mesh.vertices.size() / kFloatsPerClusterVertex);
  mesh.dirty = true;
  mesh.geometry_type = GeometryType::kTerrainCluster;
  mesh.transform = model_;  // map-local vertices placed by the entity transform

  MaterialFactoryComponent fmc;
  fmc.factory = factory_.get();
  fmc.pass_type = MaterialPassType::kDeferred;
  // Debug source mode driving terrain_cluster.wesl's debug_params.x (0 = flat
  // biome color, 1 = triangle hash, 2 = LOD level). The config hash keys only
  // override NAMES, so live value changes reuse the cached instance.
  fmc.params.uniform_overrides["debug_params"] =
      glm::vec4(static_cast<float>(debug_tint_mode_), 0.0f, 0.0f, 0.0f);
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  registry.emplace<MaterialFactoryComponent>(e, std::move(fmc));

  // Whole-map (map-local) AABB for the entity-level cull; per-range culling
  // happens inside the pass. Leaves are full-resolution, so their union is the
  // whole terrain. The draw ranges start empty -- UpdateLod fills the LOD cut.
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  int leaf_count = 0;
  for (const TerrainCluster& c : dag_.clusters) {
    if (c.level != 0) continue;
    lo = glm::min(lo, c.bounds.min);
    hi = glm::max(hi, c.bounds.max);
    ++leaf_count;
  }
  registry.emplace<MeshDrawRangesComponent>(e);
  registry.emplace<StaticMeshAabbComponent>(
      e, StaticMeshAabbComponent{Aabb::FromMinMax(lo, hi)});
  spdlog::info("ClusterTerrain: {} leaf clusters, {} levels", leaf_count,
               dag_.level_count);

  entity_ = e;
  return true;
}

void ClusterTerrain::UpdateLod(const Camera& camera, float screen_h_px) {
  if (registry_ == nullptr || entity_ == entt::null) return;
  auto* ranges = registry_->try_get<MeshDrawRangesComponent>(entity_);
  if (ranges == nullptr) return;

  // Selection runs in MAP-LOCAL space (the DAG's coords), so bring the camera
  // into that space first. With model == identity this is a no-op (mapview).
  const glm::vec3 local_cam =
      glm::vec3(inv_model_ * glm::vec4(camera.position, 1.0f));

  // Skip the reselect when nothing the cut depends on changed. Selection is a
  // pure function of camera POSITION (not orientation), tau, and screen height.
  // fov is intentionally excluded: both apps' fov is constant; if a runtime FOV
  // control is ever added, fov must join this check (it scales the metric).
  if (local_cam == last_sel_cam_pos_ && tau_px_ == last_sel_tau_ &&
      screen_h_px == last_sel_screen_h_) {
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();
  SelectClusters(dag_, local_cam, camera.fov, screen_h_px, tau_px_,
                 selected_clusters_);
  sel_time_us_ = std::chrono::duration<double, std::micro>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
  last_sel_cam_pos_ = local_cam;
  last_sel_tau_ = tau_px_;
  last_sel_screen_h_ = screen_h_px;

  // Rewrite the draw ranges in place from the selected cut (bounds carried from
  // the DAG, in map-local space -- the pass transforms them by the entity model
  // before its own frustum cull). Shared vertex/index buffers are untouched.
  ranges->ranges.clear();
  ranges->ranges.reserve(selected_clusters_.size());
  sel_level_hist_.assign(dag_.level_count, 0);
  sel_tri_count_ = 0;
  for (uint32_t cidx : selected_clusters_) {
    const TerrainCluster& c = dag_.clusters[cidx];
    ranges->ranges.push_back(
        MeshDrawRange{c.first_index, c.index_count, c.bounds});
    sel_tri_count_ += c.index_count / 3;
    if (c.level < static_cast<int>(sel_level_hist_.size())) ++sel_level_hist_[c.level];
  }
  sel_cluster_count_ = static_cast<int>(selected_clusters_.size());

  // Count the ranges surviving the CAMERA-pass frustum cull -- the real per-pass
  // ranged-draw count (selected n camera frustum). Replicates the pass's test
  // (world VP frustum vs each range's model-transformed bounds). The shadow pass
  // uses the light frustum and is not counted here.
  const Frustum cam_frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());
  sel_camera_drawn_ = 0;
  for (const MeshDrawRange& r : ranges->ranges) {
    if (cam_frustum.Intersects(r.bounds.TransformedBy(model_))) ++sel_camera_drawn_;
  }

  if (sel_cluster_count_ != last_logged_sel_count_) {
    spdlog::trace(
        "cluster LOD cut: tau={:.2f}px screen_h={:.0f} -> {} clusters ({} in "
        "camera frustum), {} tris, select {:.1f} us",
        tau_px_, screen_h_px, sel_cluster_count_, sel_camera_drawn_,
        sel_tri_count_, sel_time_us_);
    last_logged_sel_count_ = sel_cluster_count_;
  }
}

void ClusterTerrain::ApplyDebugTintMode() {
  if (registry_ == nullptr || entity_ == entt::null) return;
  auto* fmc = registry_->try_get<MaterialFactoryComponent>(entity_);
  if (fmc == nullptr) return;
  // Rewrite the per-draw override; render_textured_mesh transfers it each frame.
  fmc->params.uniform_overrides["debug_params"] =
      glm::vec4(static_cast<float>(debug_tint_mode_), 0.0f, 0.0f, 0.0f);
}

void ClusterTerrain::DrawDebugUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (!ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) return;

  // LOD budget: screen-space error in pixels. Lower = finer/more clusters. The
  // rewrite happens next UpdateLod, so the numbers below refresh a frame later.
  ImGui::SliderFloat("LOD tau (px)", &tau_px_, kMinTauPx, kMaxTauPx, "%.2f");

  // Debug tint source. Drives debug_params.x in terrain_cluster.wesl via the
  // live material override (ApplyDebugTintMode); the shader branches on it.
  static const char* const kTintItems[] = {"Shaded (biome color)",
                                            "Triangle hash", "LOD level"};
  if (ImGui::Combo("Debug tint", &debug_tint_mode_, kTintItems,
                   IM_ARRAYSIZE(kTintItems))) {
    ApplyDebugTintMode();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Shaded: albedo = per-vertex biome color.\n"
        "Triangle hash: tint by a per-triangle position hash (NOT a stable\n"
        "  per-cluster color -- that needs an engine change, deferred).\n"
        "LOD level: tint by the cluster's LOD level (hue wheel).");
  }

  // Compact stats for the current LOD cut, then a one-line per-level histogram.
  ImGui::Text("cut: %d clusters (%d in frustum)   %llu tris   select %.1f us",
              sel_cluster_count_, sel_camera_drawn_,
              static_cast<unsigned long long>(sel_tri_count_), sel_time_us_);
  std::string hist;
  for (size_t L = 0; L < sel_level_hist_.size(); ++L) {
    if (sel_level_hist_[L] == 0) continue;
    if (!hist.empty()) hist += "  ";
    hist += "L" + std::to_string(L) + ":" + std::to_string(sel_level_hist_[L]);
  }
  ImGui::TextUnformatted(hist.empty() ? "  levels: (none selected)"
                                      : ("  levels  " + hist).c_str());
}

}  // namespace badlands
