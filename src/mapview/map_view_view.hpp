#pragma once

// badlands_mapview's AppView: generates a map in-process (the mapgen pipeline),
// tessellates it into indexed kTerrainBlend chunks rendered with the biome
// texture-array terrain material, and views it with the fixed-angle
// GameCameraController. Terrain entities are created directly in the registry
// (no SceneGraph — the terrain is a raw indexed mesh, not a MeshAttachment).
//
// Hovering the mouse over the terrain draws a block/section debug grid around
// the hit point (see RebuildVisibleGrid).

#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <vector>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material_library.hpp"
#include "game/geometry/terrain_clusters.hpp"
#include "mapgen/config.hpp"
#include "mapgen/pipeline.hpp"

namespace badlands {

class MapViewView : public AppView {
 public:
  // `cfg` is the full generator config (seed/size/thresholds/terracing/...), so
  // everything --config exposes reaches the viewer. `use_cluster_terrain`
  // selects the cluster-LOD terrain (default) vs the legacy fixed-subdiv chunks
  // (the A/B baseline); the ImGui checkbox flips it at runtime.
  // `camera_height` overrides the starting camera height (0 = keep the default
  // ground-level framing); `lod_tau` seeds the screen-space-error budget in
  // pixels; `lod_tint` seeds the debug tint mode (0 shaded / 1 triangle hash /
  // 2 LOD level). `serial_build` forces the single-threaded DAG build (the perf
  // A/B baseline; default is the parallel build). All exist mainly so headless
  // --screenshot runs can frame near/far and set LOD/tint without touching the
  // interactive defaults.
  explicit MapViewView(mapgen::MapgenConfig cfg, bool use_cluster_terrain = true,
                       float camera_height = 0.0f, float lod_tau = 1.5f,
                       int lod_tint = 0, bool serial_build = false)
      : cfg_(std::move(cfg)),
        use_cluster_terrain_(use_cluster_terrain),
        tau_px_(lod_tau),
        camera_height_override_(camera_height),
        debug_tint_mode_(lod_tint),
        serial_build_(serial_build) {}

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  mapgen::MapgenConfig cfg_;

  wgpu::Device device_;
  wgpu::Queue queue_;

  MaterialLibrary matlib_;
  CubemapBuilder sky_cube_;

  // Daylight (Hosek-Wilkie sky + directional sun), same system the game uses.
  // Static rather than a running cycle: this is a map inspector, so the light
  // holds still unless you scrub it. 0.5 == noon (daylight.cpp's solar arc).
  DaylightConfig daylight_cfg_;
  float time_of_day_ = 0.5f;
  void ApplyDaylight();  // re-bakes sky + IBL; not cheap, call on change only

  // Per-biome PBR texture arrays (albedo/normal/arm), layer index = Biome enum
  // value. Held here to keep the GPU textures alive for the material's lifetime.
  MaterialLibrary::TerrainArrays terrain_arrays_;

  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // The generated map. `heightmap` is kept for mouse picking and `graph` for
  // per-section heights — both outlive Initialize, unlike the chunk tessellation
  // inputs.
  mapgen::MapArtifacts map_;

  // Nanite-style terrain cluster-LOD DAG, built from the heightmap at load.
  // Rendered (M2) as one entity whose MeshDrawRangesComponent draws all leaf
  // clusters; LOD selection over these ranges lands in a later milestone.
  TerrainClusterDag terrain_dag_;

  // Deferred material for the cluster terrain (game-owned; no engine-side
  // MaterialLibrary entry). Held for the terrain entity's lifetime.
  std::unique_ptr<MaterialInstanceFactory> cluster_factory_;

  // Which terrain path is live. Only the selected set of entities exists in the
  // registry at a time (RebuildTerrain destroys one and builds the other).
  bool use_cluster_terrain_ = true;
  entt::entity cluster_entity_ = entt::null;  // valid iff cluster terrain live
  std::vector<entt::entity> legacy_entities_;  // valid iff legacy terrain live

  // (Re)build the live terrain path from use_cluster_terrain_, destroying the
  // other. Builds the cluster entity (all leaf clusters as draw ranges) or the
  // legacy fixed-subdiv chunk entities.
  void RebuildTerrain();
  void BuildClusterTerrain();
  void BuildLegacyTerrain();

  // Per-frame LOD cut: pick the screen-space-error cluster cut for the current
  // camera and rewrite the cluster entity's draw ranges from it. No-op unless
  // the cluster terrain is live. Called from Update after the camera moves.
  void UpdateClusterLod();

  // Screen-space-error budget in pixels (the LOD knob). Higher = coarser. Seeded
  // from the constructor; adjustable via the DrawUI slider.
  float tau_px_ = 1.5f;
  // Starting camera height override (0 = default); applied once in Initialize.
  float camera_height_override_ = 0.0f;
  // Debug tint source driving the cluster material's debug_params.x uniform:
  // 0 = normal shaded (albedo = biome vertex color), 1 = per-triangle position
  // hash, 2 = LOD level. The ImGui combo flips it live; a headless run seeds it.
  int debug_tint_mode_ = 0;
  // Push debug_tint_mode_ into the live cluster entity's material override so the
  // next frame's per-draw transfer picks it up (no cache invalidation — the
  // override is per-draw data, not pipeline state).
  void ApplyDebugTintMode();
  // Viewport height in pixels, tracked by OnResize — the projection metric's
  // numerator. Seeded so the first Update (before any resize) still has a sane
  // value in headless paths.
  float screen_h_px_ = 1080.0f;

  // Scratch reused across frames so SelectClusters + the range rewrite don't
  // reallocate every frame.
  std::vector<uint32_t> selected_clusters_;
  // Last cut's stats, surfaced in DrawUI (and logged once at startup): selected
  // cluster count, drawn triangle count, and a per-level cluster histogram.
  int sel_cluster_count_ = 0;
  uint64_t sel_tri_count_ = 0;
  std::vector<int> sel_level_hist_;
  int last_logged_sel_count_ = -1;  // throttles the per-cut log line
  // SelectClusters wall time for the last cut (the CPU-selection perf number),
  // and how many selected ranges actually survive the camera-pass frustum cull
  // (selected ∩ camera frustum) — the real ranged-draw count for that pass,
  // computed CPU-side by replicating the pass test (the shadow pass, which culls
  // against the light frustum, is not instrumented here).
  double sel_time_us_ = 0.0;
  int sel_camera_drawn_ = 0;
  // Force the single-threaded DAG build (perf A/B baseline); seeded once in
  // Initialize, not runtime-toggleable (the DAG is built there).
  bool serial_build_ = false;

  DebugLineBuffer grid_;  // block + section lines, only around the hover point
  bool grid_visible_ = true;
  // Half-extent (in blocks, kBlockSizeM each) of the grid window around the hover point.
  // Runtime, not compile-time: it's a debug-view knob (ImGui slider), not a
  // structural property of the map.
  int grid_radius_blocks_ = 8;

  // Where the mouse ray last hit the terrain. `hover_valid_` is false when the
  // cursor is off the terrain (sky / past the map edge) — the grid hides.
  glm::vec3 hover_point_{0.0f};
  bool hover_valid_ = false;

  int chunk_count_ = 0;
  float map_size_m_ = 0.0f;

  // Rebuilds grid_ with block-boundary lines + highlighted section boundaries,
  // limited to a grid_radius_blocks_ window around hover_point_ (so cost is
  // independent of map size). Every line sits at its block's SECTION height, so
  // each terrace reads as one flat plane rather than stair-stepping per block.
  void RebuildVisibleGrid();

  // World height of the terrace `b` belongs to (+0 lift). Sections are the
  // flat-ish regions the grid visualizes; nodes[i].id == i, so this is a direct
  // index (see mapgen::MapArtifacts::graph).
  float SectionHeight(const mapgen::Block& b) const;
};

}  // namespace badlands
