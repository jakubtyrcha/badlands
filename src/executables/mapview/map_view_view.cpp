#include "executables/mapview/map_view_view.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/app/sdl_input_util.hpp"
#include "engine/core/ray.hpp"
#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/components/transform.hpp"
#include "engine/rendering/geometry/mesh_builder_utils.hpp"     // PushVertex
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // AABB helper
#include "engine/rendering/scene_renderer.hpp"  // debug-view selectors
#include "engine/rendering/texture_loader.hpp"  // UploadTexture2DWithMips
#include "engine/ui/editor_ui.hpp"
#include "foliage/scatter.hpp"             // GenerateFoliage
#include "game/geometry/terrain_mesh.hpp"  // RaycastTerrain(MapData)
#include "game/map/forest_test_map_generator.hpp"
#include "game/map/map_data_terrain_query.hpp"
#include "game/visual/forest_catalog.hpp"
#include "mapgen/biomes.hpp"
#include "mapview/biome_manifest.hpp"
#include "mapview/biome_splat.hpp"
#include "mapview/lake_surface.hpp"

namespace badlands {

namespace {

// Wrap the generator output in the frozen MapData contract at the raster's own
// texel spacing. Slices are ONE-HOT: the hard per-pixel biome assignment, so
// WeightsAtNode(i,j).Dominant() == the single biome and the cluster terrain's
// per-vertex color is the crisp per-texel biome. Blended slices are the game's
// symbolic generator's business.
MapData MakeOneHotMapData(const mapgen::MapArtifacts& art, glm::vec2 size_m) {
  const int sw = art.bedrock.width, sh = art.bedrock.height;
  if (sw <= 0 || sh <= 0) return {};
  const float tx = size_m.x / static_cast<float>(sw);
  const float ty = size_m.y / static_cast<float>(sh);
  if (tx <= 0.0f) return {};
  // The frozen MapData lattice has ONE spacing scalar; this wrap is the code
  // that depends on square texels, so the invariant is asserted here (the CLI
  // check in main_mapview is the user-facing error for the same contradiction).
  assert(std::abs(tx - ty) <= 1e-4f * std::max(tx, ty));
  // One more node than texels per axis: node i sits at i * tx, so the
  // lattice spans exactly the map's size_m; edge nodes clamp to the last texel.
  MapData map(sw + 1, sh + 1, tx);
  for (int j = 0; j <= sh; ++j) {
    for (int i = 0; i <= sw; ++i) {
      const int sx = std::min(i, sw - 1), sz = std::min(j, sh - 1);
      map.mutable_height(i, j) = art.heightmap.at(sx, sz);
      map.mutable_slice(art.biome.at(sx, sz), i, j) = 255;
    }
  }
  return map;
}
}  // namespace

bool MapViewView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;  // shared debug-view selectors need it

  // Map-load profiling: time each load step and log a per-step + cumulative
  // breakdown once. `log_step` accumulates into cum_ms; the closing TOTAL line is
  // the wall-clock span of the whole load (the tiny untimed bits -- camera
  // framing -- are the only gap between the two).
  using clock = std::chrono::steady_clock;
  auto since = [](clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  };
  const auto t_load = clock::now();
  double cum_ms = 0.0;
  auto log_step = [&](const char* name, double ms) {
    cum_ms += ms;
    spdlog::info("  {:<14} {:>8.1f} ms   (cum {:>8.1f} ms)", name, ms, cum_ms);
  };
  spdlog::info("map load profile (seed {}, {}x{} texels):", params_.seed,
               params_.resolution, params_.resolution);

  auto t = clock::now();
  // Start at noon, paused (an inspector holds still until you play/scrub).
  sim_clock_.speed = 0.0f;
  sim_clock_.SeekTimeOfDay(0.5f);
  ApplyDaylight();
  scene_context_.registry = &registry_;
  log_step("daylight", since(t));

  if (test_map_) {
    // --test-map: the synthetic forest map instead of the generator. The
    // generator's classify_biomes emits only Plains/Hills/Mountain, so a
    // procedural map has zero Biome::Forest coverage and the forest plopper
    // has nothing to plant into; this map exists to give it one.
    t = clock::now();
    map_size_m_ = ForestTestMapGenerator::kMapSizeM;
    params_.world_size_m = map_size_m_;
    terrain_map_ = ForestTestMapGenerator(params_.seed).Generate();
    // The splat builder wants a HARD per-texel biome raster (it derives its own
    // blend from it), so hand it the argmax of the soft slices. The 3 m blur it
    // applies re-softens the boundary for the terrain material.
    map_.biome = mapgen::Field2D<uint8_t>(terrain_map_.nodes_x(),
                                          terrain_map_.nodes_z(), 0);
    for (int j = 0; j < terrain_map_.nodes_z(); ++j) {
      for (int i = 0; i < terrain_map_.nodes_x(); ++i) {
        map_.biome.at(i, j) =
            static_cast<uint8_t>(terrain_map_.WeightsAtNode(i, j).Dominant());
      }
    }
    log_step("test map", since(t));
  } else {
    // Build the map in-process — the same generator --preview-image-only dumps,
    // so the rendered terrain and the preview PNGs can never disagree.
    t = clock::now();
    map_ = mapgen::generate_map(params_);
    log_step("mg:generate", since(t));
    map_size_m_ = params_.world_size_m;
  }

  // Lake bathymetry, logged once: the water material's extinction coefficients
  // are derived from a visibility depth in metres, so the depth distribution
  // the generator actually produces is a load-bearing input, not trivia.
  if (!test_map_) {
    std::vector<float> depths;
    depths.reserve(map_.lakes.size());
    for (const mapgen::LakeInfo& l : map_.lakes) depths.push_back(l.max_depth_m);
    std::sort(depths.begin(), depths.end());
    int wet = 0;
    for (float d : map_.water_depth.data) {
      if (d > 0.0f) ++wet;
    }
    const float wet_frac =
        map_.water_depth.data.empty()
            ? 0.0f
            : static_cast<float>(wet) /
                  static_cast<float>(map_.water_depth.data.size());
    if (depths.empty()) {
      spdlog::info("lakes: none (wet {:.2f}%)", 100.0f * wet_frac);
    } else {
      spdlog::info(
          "lakes: {}  max_depth_m min/median/max = {:.2f}/{:.2f}/{:.2f}  "
          "wet {:.2f}%",
          depths.size(), depths.front(), depths[depths.size() / 2],
          depths.back(), 100.0f * wet_frac);
    }
  }

  // Wrap the generator output in the frozen MapData contract (one-hot biomes) at
  // the raster's own texel spacing -- the input to the cluster terrain and
  // picking. The cluster LOD's job is to decimate from full detail, so the leaf
  // lattice is the finest source data (one node per texel), not a coarser mesh
  // density; LOD selection manages the triangle cost.
  if (!test_map_) {
    t = clock::now();
    terrain_map_ = MakeOneHotMapData(map_, glm::vec2(params_.world_size_m));
    log_step("map->MapData", since(t));
  }

  // Frame the camera BEFORE building the terrain, so the cluster path's initial
  // LOD selection already runs against the real camera position rather than the
  // origin. Start on the map centre at ground-level framing, matching the game's
  // own camera (game_view.cpp: pitch 50, height 42) rather than a bird's-eye
  // view. Scroll to zoom out; max_height reaches far enough to take in the whole
  // map.
  const float map_depth_m = params_.world_size_m;
  gamecam_.focus = glm::vec3(map_size_m_ * 0.5f, 0.0f, map_depth_m * 0.5f);
  gamecam_.pitch_deg = 50.0f;
  gamecam_.height = 42.0f;
  gamecam_.min_height = 5.0f;
  gamecam_.max_height = std::max(400.0f, map_size_m_);
  // Headless framing override (--camera-height): clamp into the controller's
  // range so a far shot can pull well back without escaping it.
  if (camera_height_override_ > 0.0f) {
    gamecam_.max_height = std::max(gamecam_.max_height, camera_height_override_);
    gamecam_.height = std::clamp(camera_height_override_, gamecam_.min_height,
                                 gamecam_.max_height);
  }
  gamecam_.UpdateCamera(camera_);

  // Terrain materials: one PBR pack per biome, keyed by name so a renamed or
  // reordered manifest entry fails loudly instead of mis-mapping a biome.
  t = clock::now();
  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  std::vector<std::string> pack_dirs;
  if (!ResolveBiomePacks("assets/materials/terrain_biomes.json", pack_dirs)) {
    spdlog::error("MapViewView: failed to resolve biome packs");
    return false;
  }
  terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  if (!matlib_.ok()) {
    spdlog::error("MapViewView: terrain arrays failed to build");
    return false;
  }
  log_step("biome packs", since(t));

  // Biome splat: the per-biome blend weights, sampled by world XZ in the
  // fragment stage rather than carried on the vertices, so the coarsest LOD
  // cluster still gets full-resolution biome detail.
  t = clock::now();
  // Texel size comes from the biome raster's OWN width, not params_.resolution:
  // the blur radius and the world->UV transform below must be derived from the
  // same source, or they would disagree silently if the generator ever emitted
  // the biome raster at a different resolution than requested.
  const float splat_texel_m =
      map_.biome.width > 0
          ? params_.world_size_m / static_cast<float>(map_.biome.width)
          : 0.0f;
  const BiomeSplat splat = BuildBiomeSplat(map_.biome, splat_texel_m);
  if (splat.empty()) {
    spdlog::error("MapViewView: empty biome splat");
    return false;
  }
  splat0_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots0.data())
                     .view;
  splat1_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots1.data())
                     .view;
  if (!splat0_view_ || !splat1_view_) {
    spdlog::error("MapViewView: biome splat upload failed");
    return false;
  }
  // Trilinear + CLAMP. Mips matter: at max zoom one screen pixel covers several
  // map texels, and unmipped weights alias into a shimmering biome mosaic.
  wgpu::SamplerDescriptor splat_sd = {};
  splat_sd.minFilter = wgpu::FilterMode::Linear;
  splat_sd.magFilter = wgpu::FilterMode::Linear;
  splat_sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  splat_sd.addressModeU = wgpu::AddressMode::ClampToEdge;
  splat_sd.addressModeV = wgpu::AddressMode::ClampToEdge;
  splat_sampler_ = device_.CreateSampler(&splat_sd);
  // world XZ in [0, size] -> texel CENTRES in [0.5/N, 1 - 0.5/N].
  const float inv_n = 1.0f / static_cast<float>(splat.width);
  const float splat_scale = (1.0f - inv_n) / params_.world_size_m;
  const glm::vec4 splat_uv(splat_scale, splat_scale, 0.5f * inv_n, 0.5f * inv_n);
  log_step("biome splat", since(t));

  // Build the shared cluster-LOD terrain (identity model -- mapview vertices are
  // absolute world coords). --serial-build forces the single-threaded DAG build
  // for the perf A/B (both produce a bit-identical DAG). Seed the debug tint from
  // --lod-tint so a headless run renders tinted on frame one.
  cluster_terrain_.debug_tint_mode() = initial_tint_;
  TerrainClusterParams cluster_params;
  cluster_params.parallel_build = !serial_build_;
  t = clock::now();
  if (!cluster_terrain_.Build(terrain_map_, ctx, registry_, glm::mat4(1.0f),
                              cluster_params, terrain_arrays_,
                              matlib_.shared_sampler(), splat0_view_,
                              splat1_view_, splat_sampler_, splat_uv)) {
    spdlog::error("MapViewView: cluster terrain build failed");
    return false;
  }
  // Seed the LOD cut once so the first rendered frame (headless --screenshot
  // renders after a single Update) already draws the selected cut.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);
  log_step("cluster terrain", since(t));

  // Still lake water. The surface deliberately overlaps each shore and runs
  // under the terrain; water tests depth without writing it, so the buried ring
  // is rejected in hardware -- and that overlap is what keeps a later wave
  // displacement from opening a gap at the waterline.
  t = clock::now();
  water_factory_ =
      BuildStillWaterForwardFactory(ctx.device, ctx.queue, ctx.pipeline_gen);
  if (!water_factory_) {
    spdlog::error("MapViewView: water factory build failed");
    return false;
  }
  // The test map has no lakes at all (its water level sits below the lowest
  // ground), so BuildLakeSurfaceTriangles has nothing to read -- skip it rather
  // than hand it the empty artifacts left over from the skipped generator run.
  const std::vector<glm::vec3> water_tris =
      test_map_ ? std::vector<glm::vec3>{}
                : BuildLakeSurfaceTriangles(map_, params_.world_size_m);
  if (!water_tris.empty()) {
    std::vector<float> v;
    v.reserve(water_tris.size() * kTexturedMeshFloatsPerVertex);
    for (const glm::vec3& p : water_tris) {
      // uv = world XZ, normal +Y, tangent +X -- a flat plane needs no more.
      PushVertex(v, p, glm::vec2(p.x, p.z), glm::vec3(0.0f, 1.0f, 0.0f),
                 glm::vec3(1.0f, 0.0f, 0.0f));
    }
    // Created directly in the registry, mirroring what SceneGraph's
    // MeshAttachment path emplaces (mesh + AABB + material + the pass tag).
    // A SceneGraph is not usable here: SyncToRegistry clears the registry.
    const entt::entity e = registry_.create();
    registry_.emplace<Transform>(e).matrix = glm::mat4(1.0f);
    auto& mesh = registry_.emplace<StaticTexturedMeshComponent>(e);
    mesh.vertex_count =
        static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
    mesh.dirty = true;
    mesh.geometry_type = GeometryType::kTexturedMesh;
    registry_.emplace<StaticMeshAabbComponent>(
        e, StaticMeshAabbComponent{
               .local = ComputeLocalAabbFromVertices(
                   v, kTexturedMeshFloatsPerVertex)});
    mesh.vertices = std::move(v);

    MaterialFactoryComponent fmc;
    fmc.factory = water_factory_.get();
    fmc.pass_type = MaterialPassType::kForwardTransparent;
    fmc.params = StillLakeWaterParams();
    fmc.config_hash = ComputeFactoryConfigHash(fmc);
    registry_.emplace<MaterialFactoryComponent>(e, std::move(fmc));
    registry_.emplace<ForwardTransparentRenderable>(e);
  }
  spdlog::info("water: {} triangles over {} lakes", water_tris.size() / 3,
               map_.lakes.size());
  log_step("water", since(t));

  // The forest. Placement is pure CPU over the frozen MapData contract, so
  // nothing here knows how the map was made -- a generated map simply reports
  // zero Forest coverage and plants nothing.
  t = clock::now();
  ForestCatalog forest_catalog;
  if (!BuildForestCatalog(forest_catalog)) {
    spdlog::error("MapViewView: forest catalog build failed");
    return false;
  }

  const MapDataTerrainQuery forest_query(terrain_map_, mapgen::Biome::Forest);
  foliage::FoliageGenParams foliage_params;
  foliage_params.seed = params_.seed;
  foliage_params.origin_m = glm::vec2(0.0f);
  foliage_params.size_m = glm::vec2(terrain_map_.size_x_m(),
                                    terrain_map_.size_z_m());
  foliage::FoliageField foliage_field = foliage::GenerateFoliage(
      forest_catalog.type, forest_query, foliage_params);
  log_step("foliage place", since(t));

  t = clock::now();
  if (!forest_.Build(ctx.device, ctx.queue, *ctx.pipeline_gen,
                     std::move(forest_catalog), std::move(foliage_field))) {
    spdlog::error("MapViewView: forest renderer build failed");
    return false;
  }
  forest_field_ = forest_.instanced_field();
  if (forest_field_) {
    forest_.Update(camera_, scene_context_.sun_direction);
    scene_context_.instanced_fields = &forest_field_;
    scene_context_.instanced_field_count = 1;
  }
  log_step("forest build", since(t));

  spdlog::info("map load: {:.1f} ms total  ({}x{} texels)", since(t_load),
               params_.resolution, params_.resolution);

  return true;
}

void MapViewView::ApplyDaylight() {
  const DaylightState state =
      ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());
  ApplyDaylightEnvironment(state, daylight_cfg_, device_, queue_, sky_cube_,
                           scene_context_);
}

void MapViewView::HandleEvent(const SDL_Event& event, int /*width*/,
                              int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  // Mouse coords are logical points; HandleEvent's width/height are physical
  // pixels. EventWindowLogicalSize keeps both in one space (see its docs).
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.motion.windowID, screen)) {
        hover_valid_ = false;
        return;
      }
      const Ray ray = ScreenPointToRay(
          camera_, glm::vec2(event.motion.x, event.motion.y), screen);
      hover_valid_ = RaycastTerrain(terrain_map_, ray, hover_point_);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
      const glm::vec2 pixel(event.wheel.mouse_x, event.wheel.mouse_y);
      ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel), pixel,
                   screen);
      // The camera moved under a stationary cursor, so the hover point is stale
      // -- re-pick now rather than waiting for the next motion event (which may
      // never come if the user is only scrolling).
      const Ray ray = ScreenPointToRay(camera_, pixel, screen);
      hover_valid_ = RaycastTerrain(terrain_map_, ray, hover_point_);
      break;
    }
    default:
      break;
  }
}

void MapViewView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Advance the shared clock; when it's running, move the sun (paused =>
  // holds). The daylight re-bake is throttled implicitly: ApplyDaylight only
  // runs while time actually moves.
  const double sim_dt = sim_clock_.Advance(dt);
  if (sim_dt > 0.0) ApplyDaylight();

  if (keyboard_state != nullptr && ImGui::GetCurrentContext() != nullptr &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    glm::vec2 dir(0.0f);
    if (keyboard_state[SDL_SCANCODE_W] || keyboard_state[SDL_SCANCODE_UP]) dir.y -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_S] || keyboard_state[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    if (keyboard_state[SDL_SCANCODE_A] || keyboard_state[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_D] || keyboard_state[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
    gamecam_.PanKeyboard(dir, dt);  // zoom-scaled; no-op when dir is zero
  }
  gamecam_.UpdateCamera(camera_);

  // Re-select the LOD cluster cut for the new camera and rewrite the draw
  // ranges. Cheap flat pass over the DAG; no buffer re-upload.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);

  // Coarse-cull the 32 m foliage cells and re-upload only if the visible set
  // changed (see forest_renderer.hpp). No-op for a forest-less map.
  forest_.Update(camera_, scene_context_.sun_direction);
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d texels  %.0fx%.0f m", params_.seed,
              params_.resolution, params_.resolution, params_.world_size_m,
              params_.world_size_m);
  cluster_terrain_.DrawDebugUI();
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  if (hover_valid_) {
    const std::string_view bn = mapgen::biome_name(
        terrain_map_.DominantBiomeAt(hover_point_.x, hover_point_.z));
    ImGui::Text("hover: (%.1f, %.1f, %.1f)  %.*s", hover_point_.x,
                hover_point_.y, hover_point_.z, static_cast<int>(bn.size()),
                bn.data());
  } else {
    ImGui::TextUnformatted("hover: (off terrain)");
  }
  ImGui::End();

  // Shared sim/daylight/debug controls (same helpers the game uses). Re-bake the
  // sky immediately on a scrub or a config edit so it's visible without waiting
  // for the clock; while playing, Update already re-bakes as time advances.
  ImGui::Begin("Sim / Daylight / Debug");
  if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawSimClockControls(sim_clock_)) ApplyDaylight();
  }
  if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawDaylightEditor(daylight_cfg_)) ApplyDaylight();
  }
  if (scene_renderer_ != nullptr) {
    if (ImGui::CollapsingHeader("Debug Views")) {
      EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
      EditorUI::DrawShadowDebugSelector(*scene_renderer_);
    }
  }
  EditorUI::DrawStats(dt_);
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  // The LOD screen-space-error metric is in pixels, so it needs the viewport
  // height in pixels -- exactly what OnResize carries (physical pixels windowed,
  // the capture height headless).
  if (height > 0) screen_h_px_ = static_cast<float>(height);
}

}  // namespace badlands
