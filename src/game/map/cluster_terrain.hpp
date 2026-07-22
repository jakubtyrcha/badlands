#pragma once

// Shared, app-agnostic cluster-LOD terrain module. Encapsulates the whole
// MapData -> render pipeline so an app only supplies MapData + its render
// context/registry:
//   * Build   -- builds the Nanite-style cluster DAG from the frozen MapData
//                contract, its own vertex-color material factory, and one
//                terrain entity (shared mesh + empty MeshDrawRangesComponent +
//                whole-map AABB) into the registry.
//   * UpdateLod -- re-selects the screen-space-error LOD cut for the camera and
//                rewrites the entity's draw ranges. Call each frame.
//   * DrawDebugUI -- the "Terrain" ImGui section (tau slider + debug tint combo
//                + cut stats), so every app gets identical controls.
//
// The DAG is built in MAP-LOCAL space (map coords). `model` is the entity's
// world transform: identity for mapview, a centering transform for game_view.
// UpdateLod transforms the camera into the entity's local space via
// inverse(model) before the screen-space metric, so a non-identity model works
// unchanged. Both apps share this one module (game_view wired in M7.2); it does
// NOT live inside any AppView.

#include <cstdint>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "game/geometry/terrain_clusters.hpp"  // TerrainClusterDag, MapData, tau consts

namespace badlands {

struct RenderContext;
class Camera;
class MaterialInstanceFactory;

class ClusterTerrain {
 public:
  ClusterTerrain();
  ~ClusterTerrain();
  ClusterTerrain(const ClusterTerrain&) = delete;
  ClusterTerrain& operator=(const ClusterTerrain&) = delete;

  // Build the DAG from `map`, the vertex-color material factory, and one terrain
  // entity into `registry`. `model` is the entity's world transform (identity
  // for mapview; a centering transform for game_view in M7.2). `params` carries
  // the build knobs (e.g. parallel_build). Returns false if the material factory
  // fails to build. Seeds an initial LOD cut so the first rendered frame draws.
  bool Build(const MapData& map, const RenderContext& ctx,
             entt::registry& registry, const glm::mat4& model = glm::mat4(1.0f),
             const TerrainClusterParams& params = {});

  // Re-select the LOD cut for `camera` and rewrite the entity's draw ranges.
  // Call each frame. Early-outs when the cut-affecting inputs (camera position in
  // the entity's local space, tau, screen height) are all unchanged.
  void UpdateLod(const Camera& camera, float screen_h_px);

  // The "Terrain" ImGui section (tau slider + debug tint combo + cut stats).
  // Safe to call inside an existing window (draws a CollapsingHeader).
  void DrawDebugUI();

  float& tau_px() { return tau_px_; }
  int& debug_tint_mode() { return debug_tint_mode_; }
  entt::entity entity() const { return entity_; }

 private:
  // Push debug_tint_mode_ into the live entity's material override so the next
  // frame's per-draw transfer picks it up (per-draw data, not pipeline state).
  void ApplyDebugTintMode();

  TerrainClusterDag dag_;
  std::unique_ptr<MaterialInstanceFactory> factory_;

  entt::registry* registry_ = nullptr;  // non-owning; set in Build
  entt::entity entity_ = entt::null;    // the terrain entity

  // Entity world transform + its inverse (camera -> map-local for the metric).
  glm::mat4 model_{1.0f};
  glm::mat4 inv_model_{1.0f};

  // Screen-space-error budget in pixels (the LOD knob). Higher = coarser.
  float tau_px_ = kDefaultTauPx;
  // Debug tint driving the material's debug_params.x: 0 shaded (biome color),
  // 1 per-triangle position hash, 2 LOD level.
  int debug_tint_mode_ = 0;

  // Scratch + last-cut stats (surfaced by DrawDebugUI, logged on change).
  std::vector<uint32_t> selected_clusters_;
  int sel_cluster_count_ = 0;
  uint64_t sel_tri_count_ = 0;
  std::vector<int> sel_level_hist_;
  int last_logged_sel_count_ = -1;
  double sel_time_us_ = 0.0;
  int sel_camera_drawn_ = 0;

  // Inputs of the last cut (in map-local space); seeded so the first UpdateLod
  // always recomputes. Selection is a pure function of exactly these.
  glm::vec3 last_sel_cam_pos_{0.0f};
  float last_sel_tau_ = -1.0f;
  float last_sel_screen_h_ = -1.0f;
};

}  // namespace badlands
