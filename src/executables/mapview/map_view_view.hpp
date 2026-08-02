#pragma once

// badlands_mapview's AppView: fetches a patch (mapgen::PatchSource -- rasters
// on disk, a synthetic source, or a coarse-world cut, all through the same
// interface -- or the synthetic --test-map forest fixture), wraps it in the
// frozen MapData contract, and renders it as Nanite-style cluster-LOD terrain
// (the shared ClusterTerrain module) with the fixed-angle GameCameraController. Terrain is
// one entity holding the shared cluster mesh; a MeshDrawRangesComponent
// carries the per-frame LOD cut. Entities are created directly in the
// registry (no SceneGraph -- the terrain is a raw indexed mesh, not a
// MeshAttachment).
//
// Beyond the terrain the view carries a shared SimClock driving the sun and
// cursor-anchored zoom. Hovering the mouse over the terrain shows its world
// position + dominant biome. (The old fog-emitter system was removed pending
// a rewrite.)

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/app/sim_clock.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/water_material.hpp"
#include "game/map/cluster_terrain.hpp"
#include "game/map/map_data.hpp"
#include "game/visual/forest_renderer.hpp"
#include "mapgen/patch_source.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/river_carve.hpp"

namespace badlands {

class SceneRenderer;

class MapViewView : public AppView {
 public:
  // `request` + `source` are how the CLI's chosen map reaches the viewer:
  // `source_->Fetch(request_)` in Initialize is the ONLY way a patch enters
  // this class (see mapgen/patch_source.hpp -- stage 3's whole view of where a
  // map comes from). `foliage_seed` seeds foliage placement (and, with
  // `test_map`, the synthetic forest layout) -- it is NOT a terrain seed, since
  // terrain now comes from the source. `camera_height` overrides the starting
  // camera height (0 = keep the default ground-level framing); `lod_tint`
  // seeds the cluster debug tint (0 shaded / 1 triangle hash / 2 LOD level).
  // `serial_build` forces the single-threaded DAG build (the perf A/B
  // baseline; default is the parallel build). The overrides exist mainly so
  // headless --screenshot runs can frame near/far and set the tint without
  // touching the interactive defaults.
  //
  // TWO ways to get a map, and they are mutually exclusive (main_mapview
  // requires exactly one):
  //   - `test_map` false: `source` is fetched with `request`, so a map coming
  //     from any PatchSource -- rasters on disk, a synthetic patch, a coarse
  //     world -- renders through the identical path.
  //   - `test_map` true: the synthetic forest map
  //     (game/map/forest_test_map_generator.hpp), and `source`/`request` are
  //     ignored. It exists to give the forest plopper something to plant into
  //     -- see Initialize.
  explicit MapViewView(mapgen::PatchRequest request,
                       std::shared_ptr<const mapgen::PatchSource> source,
                       uint32_t foliage_seed, float camera_height = 0.0f,
                       int lod_tint = 0, bool serial_build = false,
                       bool test_map = false)
      : request_(request),
        source_(std::move(source)),
        foliage_seed_(foliage_seed),
        camera_height_override_(camera_height),
        initial_tint_(lod_tint),
        serial_build_(serial_build),
        test_map_(test_map) {}

  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  mapgen::PatchRequest request_;
  std::shared_ptr<const mapgen::PatchSource> source_;  // null when test_map_
  uint32_t foliage_seed_ = 1;

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;  // shared, owned by the app
  float dt_ = 0.0f;                          // last real frame dt (for the FPS line)

  CubemapBuilder sky_cube_;

  // Daylight (Hosek-Wilkie sky + directional sun), same system the game uses,
  // driven by the shared SimClock (play/pause/speed + scrub). Seeded to noon;
  // starts paused (an inspector).
  DaylightConfig daylight_cfg_;
  SimClock sim_clock_;
  void ApplyDaylight();  // re-bakes sky + IBL; not cheap, call on change only

  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  // The fetched/synthetic patch. `height` is kept for mouse picking; outlives
  // Initialize.
  mapgen::PatchData patch_;
  // The map wrapped in the frozen MapData contract (one-hot biome slices at
  // the raster's own texel spacing) -- what the cluster terrain builder and
  // mouse picking read.
  MapData terrain_map_;

  // Terrain materials: one PBR pack per biome (layer index == Biome enum),
  // resolved through assets/materials/terrain_biomes.json. The library also owns
  // the shared trilinear+aniso sampler the arrays must be read through.
  MaterialLibrary matlib_;
  MaterialLibrary::TerrainArrays terrain_arrays_;

  // Biome weights as a splat texture sampled by world XZ: 8 slots across two
  // RGBA8 planes. Held as views (each keeps its texture alive) for the lifetime
  // of the terrain material that binds them. Its sampler CLAMPS -- the splat
  // covers the map's own extent, so repeating would fold edge onto edge.
  wgpu::TextureView splat0_view_, splat1_view_;
  wgpu::Sampler splat_sampler_;

  // The shared cluster-LOD terrain module: owns the DAG, its material factory,
  // the terrain entity, the per-frame LOD cut, and the Terrain debug UI. Built
  // with an identity model (mapview vertices are absolute world).
  ClusterTerrain cluster_terrain_;

  // Still lake water: one static forward-transparent entity, created DIRECTLY
  // in the registry like the cluster terrain rather than through a SceneGraph.
  // SceneGraph::SyncToRegistry starts with registry.clear(), so a graph sharing
  // this registry would wipe the terrain entity every frame -- mapview owns its
  // entities outright, and there is nothing here for a scene graph to do.
  std::unique_ptr<MaterialInstanceFactory> water_factory_;

  // The forest: placed instances filed into 32 m cells, drawn as one GPU-culled
  // instanced field of ~28 tree models. Empty (and costing nothing) unless
  // --test-map is on, since a procedural map has no Forest biome to plant into.
  ForestRenderer forest_;
  // The single instanced field pointer SceneContext::instanced_fields points
  // at. A member because the context holds it by pointer across frames.
  InstancedMeshField* forest_field_ = nullptr;

  // Where the mouse ray last hit the terrain. `hover_valid_` is false when the
  // cursor is off the terrain (sky / past the map edge) -- the hover UI hides.
  glm::vec3 hover_point_{0.0f};
  bool hover_valid_ = false;

  float map_size_m_ = 0.0f;

  // River network, carried by the patch (PatchData::rivers -- see
  // mapgen/patch_data.hpp), then refitted reach by reach as a chain of
  // CIRCULAR ARCS (mapgen/river_arcs.hpp), CARVED into the terrain, and
  // floated as the channel water surface inside that carved cavity.
  //
  // This replaced a debug-LINE layer, and the two cannot coexist: they share a
  // centreline, and a screen-space line 1-4 px wide covers the channel at every
  // camera height where you would look at the map. The line layer was the
  // placeholder; what it carried and this does not is Strahler order, traded
  // for true width.
  std::vector<mapgen::RiverArcChain> river_arcs_;
  bool show_rivers_ = true;
  // One static forward-transparent entity (the lake water's material), created
  // directly in the registry like the lake surface. Held so the visibility
  // toggle can destroy and rebuild it: there is no per-entity visibility flag in
  // the registry, and dropping the mesh is cheaper than carrying one.
  entt::entity river_mesh_ = entt::null;
  void BuildRiverMesh();

  // THE ADAPTER: the only place a river meets the terrain builder.
  //
  // The carve owns the corridor mask + the carved-height field; the exponent
  // grid and detail field below are that mask restated in the builder's own
  // generic vocabulary ("this quad wants 2^k subdivision, and here is the
  // height function"). Downstream -- ClusterTerrain, BuildTerrainClusterDag --
  // nothing knows a river exists.
  //
  // ADDRESS STABILITY is why the carve is a unique_ptr rather than a value or
  // an optional: `river_detail_.height_at` closes over a raw pointer to it and
  // the DAG build calls that millions of times, so the carve must outlive the
  // build and must never move. The exponent vector is a member for the same
  // reason -- TerrainDetailField::level points straight at its storage.
  std::unique_ptr<mapgen::RiverCarve> river_carve_;
  std::vector<uint8_t> river_detail_level_;
  TerrainDetailField river_detail_;

  // Starting camera height override (0 = default); applied once in Initialize.
  float camera_height_override_ = 0.0f;
  // Debug tint seed for the cluster terrain (headless --lod-tint); pushed into
  // cluster_terrain_ before Build so frame one renders tinted.
  int initial_tint_ = 0;
  // Force the single-threaded cluster DAG build (perf A/B baseline); seeded once
  // in Initialize, not runtime-toggleable (the DAG is built there).
  bool serial_build_ = false;
  // --test-map: use the synthetic forest map instead of fetching from `source_`.
  bool test_map_ = false;
  // Viewport height in pixels, tracked by OnResize -- the LOD screen-space-error
  // metric's numerator. Seeded so the first Update (before any resize) still has
  // a sane value in headless paths.
  float screen_h_px_ = 1080.0f;
};

}  // namespace badlands
