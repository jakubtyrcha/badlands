#pragma once

// badlands_mapview's AppView: generates a map in-process via
// mapgen::generate_map, wraps it in the frozen MapData contract, and renders
// it as Nanite-style cluster-LOD terrain (the shared ClusterTerrain module)
// with the fixed-angle GameCameraController. Terrain is one entity holding the
// shared cluster mesh; a MeshDrawRangesComponent carries the per-frame LOD
// cut. Entities are created directly in the registry (no SceneGraph -- the
// terrain is a raw indexed mesh, not a MeshAttachment).
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
#include "mapgen/generator.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/window_rivers.hpp"

namespace badlands {

class SceneRenderer;

class MapViewView : public AppView {
 public:
  // `params` is the generator params (seed/resolution/size), so everything the
  // CLI exposes reaches the viewer. `camera_height` overrides the starting
  // camera height (0 = keep the default ground-level framing); `lod_tint`
  // seeds the cluster debug tint (0 shaded / 1 triangle hash / 2 LOD level).
  // `serial_build` forces the single-threaded DAG build (the perf A/B
  // baseline; default is the parallel build). The overrides exist mainly so
  // headless --screenshot runs can frame near/far and set the tint without
  // touching the interactive defaults.
  //
  // THREE ways to get a map, and they are mutually exclusive:
  //   - neither flag: mapgen::generate_map, the procedural generator.
  //   - `load_dir` non-empty: rasters on disk (mapgen::load_map), so a world
  //     simulated outside this process renders through the identical path.
  //     main_mapview parses the manifest FIRST and writes
  //     params.resolution/world_size_m from it, so every params_ use below stays
  //     correct without a second source of truth.
  //   - `test_map`: the synthetic forest map
  //     (game/map/forest_test_map_generator.hpp). It exists because
  //     mapgen::classify_biomes emits no Biome::Forest, so a generated map has
  //     no forest for the plopper to plant into -- see Initialize.
  explicit MapViewView(mapgen::MapGenParams params, float camera_height = 0.0f,
                       int lod_tint = 0, bool serial_build = false,
                       std::string load_dir = {}, bool test_map = false)
      : params_(params),
        load_dir_(std::move(load_dir)),
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
  mapgen::MapGenParams params_;
  std::string load_dir_;  // non-empty => load rasters instead of generating

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

  // The generated map. `heightmap` is kept for mouse picking, `bedrock` for
  // previews/erosion later -- both outlive Initialize.
  mapgen::MapArtifacts map_;
  // The generator output wrapped in the frozen MapData contract (one-hot biome
  // slices at the raster's own texel spacing) -- what the cluster terrain
  // builder and mouse picking read.
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

  // River network, built at load from the routed window (see window_rivers.hpp),
  // then refitted reach by reach as a chain of CIRCULAR ARCS
  // (mapgen/river_arcs.hpp) and swept into a draped ribbon mesh.
  //
  // This replaced a debug-LINE layer, and the two cannot coexist: they share a
  // centreline, and a screen-space line 1-4 px wide covers a 1.5-4 m ribbon at
  // every camera height where you would look at the map. The line layer was the
  // placeholder; what it carried and the ribbon does not is Strahler order,
  // which the ribbon trades for true width.
  mapgen::WindowRivers rivers_;
  std::vector<mapgen::RiverArcChain> river_arcs_;
  bool show_rivers_ = true;
  // One static deferred entity, created directly in the registry like the lake
  // water. Held so the visibility toggle can destroy and rebuild it: there is
  // no per-entity visibility flag in the registry, and dropping the mesh is
  // cheaper than carrying one.
  entt::entity river_mesh_ = entt::null;
  void BuildRiverMesh();

  // Starting camera height override (0 = default); applied once in Initialize.
  float camera_height_override_ = 0.0f;
  // Debug tint seed for the cluster terrain (headless --lod-tint); pushed into
  // cluster_terrain_ before Build so frame one renders tinted.
  int initial_tint_ = 0;
  // Force the single-threaded cluster DAG build (perf A/B baseline); seeded once
  // in Initialize, not runtime-toggleable (the DAG is built there).
  bool serial_build_ = false;
  // --test-map: use the synthetic forest map instead of running the generator.
  bool test_map_ = false;
  // Viewport height in pixels, tracked by OnResize -- the LOD screen-space-error
  // metric's numerator. Seeded so the first Update (before any resize) still has
  // a sane value in headless paths.
  float screen_h_px_ = 1080.0f;
};

}  // namespace badlands
