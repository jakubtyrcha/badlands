#include "mapview/map_view_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/app/sdl_input_util.hpp"
#include "engine/core/ray.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // ComputeLocalAabbFromVertices
#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/pipeline.hpp"
#include "mapview/biome_manifest.hpp"  // ResolveBiomePacks

namespace badlands {

namespace {
constexpr int kChunkBlocks = 16;  // N x N blocks per chunk (kBlockSizeM each)
constexpr int kSubdiv = 2;        // subgrid cells per block edge
constexpr const char* kBiomeManifestPath = "assets/materials/terrain_biomes.json";
}  // namespace

bool MapViewView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  ApplyDaylight();
  scene_context_.registry = &registry_;

  // One PBR pack per biome, layer index = Biome enum value. The mapping is data
  // (assets/materials/terrain_biomes.json); the engine only sees "N packs".
  std::vector<std::string> pack_dirs;
  if (!ResolveBiomePacks(kBiomeManifestPath, pack_dirs)) return false;
  terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  if (!matlib_.ok()) {
    spdlog::error("MapViewView: terrain material packs failed to load");
    return false;
  }
  // Generate the map in-process — the same pipeline --preview-image-only dumps,
  // so the rendered terrain and the preview PNGs can never disagree.
  std::string err;
  if (!mapgen::run_pipeline(cfg_, "scripts/mapgen/fields.noiser", map_, err)) {
    spdlog::error("MapViewView: {}", err);
    return false;
  }
  map_size_m_ = static_cast<float>(cfg_.width) * mapgen::kMetersPerSample;

  // Build the terrain cluster-LOD DAG right after mapgen. M2 renders all its
  // leaf clusters; LOD selection over the ranges lands in a later milestone.
  terrain_dag_ = BuildTerrainClusterDag(map_.heightmap, map_.biomes.pixel);

  // Deferred cluster-terrain material factory (game-owned; mirrors the engine's
  // terrain_blend descriptor but for kTerrainCluster geometry + the flat-color
  // fs_gbuffer entry, and needs no textures — the vertex color is the albedo).
  FactoryDescriptor cluster_desc;
  cluster_desc.shader_name = "terrain_cluster";
  cluster_desc.shader_path = "material/terrain_cluster.wesl";
  cluster_desc.fs_entry = "fs_gbuffer";
  cluster_desc.supported_pass_types = {MaterialPassType::kDeferred};
  cluster_desc.supported_geometry_types = {GeometryType::kTerrainCluster};
  cluster_desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                                GBuffer::kMaterialFormat};
  cluster_desc.depth_format = GBuffer::kDepthFormat;
  cluster_factory_ = BuildMaterialInstanceFactory(cluster_desc, ctx.device,
                                                  ctx.queue, ctx.pipeline_gen);
  if (!cluster_factory_) {
    spdlog::error("MapViewView: failed to build terrain_cluster material factory");
    return false;
  }

  // Frame the camera BEFORE building the terrain, so the cluster path's initial
  // LOD selection (BuildClusterTerrain -> UpdateClusterLod) already runs against
  // the real camera position rather than the origin.
  //
  // Start on the map centre at ground-level framing, matching the game's own
  // camera (game_view.cpp: pitch 50, height 42) rather than a bird's-eye view —
  // this is meant to show the map as it will actually be played. Scroll to zoom
  // out; max_height reaches far enough to take in the whole map.
  const float map_depth_m = static_cast<float>(cfg_.height) *
                            mapgen::kMetersPerSample;
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

  RebuildTerrain();
  spdlog::info("MapViewView: {}x{} map, {} sections", cfg_.width, cfg_.height,
               map_.graph.nodes.size());

  // The grid follows the mouse, so there is nothing to draw until the cursor is
  // over the terrain (Update wires debug_lines once hover_valid_).
  return true;
}

void MapViewView::RebuildTerrain() {
  // Tear down whichever terrain path is currently live.
  if (cluster_entity_ != entt::null) {
    registry_.destroy(cluster_entity_);
    cluster_entity_ = entt::null;
  }
  for (entt::entity e : legacy_entities_) registry_.destroy(e);
  legacy_entities_.clear();
  chunk_count_ = 0;

  if (use_cluster_terrain_) {
    BuildClusterTerrain();
  } else {
    BuildLegacyTerrain();
  }
}

void MapViewView::BuildClusterTerrain() {
  // One entity holds the whole shared cluster mesh; MeshDrawRangesComponent
  // draws each leaf cluster as its own culled DrawIndexed range. The DAG is
  // kept intact (copied, not moved) for the later LOD-selection milestone.
  const entt::entity e = registry_.create();
  auto& mesh = registry_.emplace<StaticTexturedMeshComponent>(e);
  mesh.vertices = terrain_dag_.vertices;
  mesh.indices = terrain_dag_.indices;
  mesh.vertex_count = static_cast<uint32_t>(terrain_dag_.vertices.size() /
                                            kFloatsPerClusterVertex);
  mesh.dirty = true;
  mesh.geometry_type = GeometryType::kTerrainCluster;
  mesh.transform = glm::mat4(1.0f);  // vertices are absolute world coords

  MaterialFactoryComponent fmc;
  fmc.factory = cluster_factory_.get();
  fmc.pass_type = MaterialPassType::kDeferred;
  // Debug source mode driving terrain_cluster.wesl's debug_params.x (0 = flat
  // biome color, 1 = triangle hash, 2 = LOD level). Seeded from
  // debug_tint_mode_ so a headless --lod-tint run renders tinted on frame one;
  // the DrawUI combo flips it live via ApplyDebugTintMode. The config hash keys
  // only override NAMES, so live value changes reuse the cached instance.
  fmc.params.uniform_overrides["debug_params"] =
      glm::vec4(static_cast<float>(debug_tint_mode_), 0.0f, 0.0f, 0.0f);
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  registry_.emplace<MaterialFactoryComponent>(e, std::move(fmc));

  // All leaf clusters (level 0) as draw ranges, plus a whole-map AABB for the
  // entity-level cull (per-range culling happens inside the pass). Leaves are
  // full-resolution, so their union covers the entire terrain.
  MeshDrawRangesComponent ranges;
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  for (const TerrainCluster& c : terrain_dag_.clusters) {
    if (c.level != 0) continue;
    ranges.ranges.push_back(MeshDrawRange{c.first_index, c.index_count, c.bounds});
    lo = glm::min(lo, c.bounds.min);
    hi = glm::max(hi, c.bounds.max);
  }
  const size_t leaf_range_count = ranges.ranges.size();
  registry_.emplace<MeshDrawRangesComponent>(e, std::move(ranges));
  registry_.emplace<StaticMeshAabbComponent>(
      e, StaticMeshAabbComponent{Aabb::FromMinMax(lo, hi)});
  spdlog::info("MapViewView: cluster terrain, {} leaf draw ranges",
               leaf_range_count);

  cluster_entity_ = e;

  // Seed the LOD cut once so the first rendered frame (headless --screenshot
  // renders after a single Update) already draws the selected cut, not the raw
  // leaf set above. UpdateClusterLod logs the resulting stats.
  UpdateClusterLod();
}

void MapViewView::UpdateClusterLod() {
  if (!use_cluster_terrain_ || cluster_entity_ == entt::null) return;
  auto* ranges = registry_.try_get<MeshDrawRangesComponent>(cluster_entity_);
  if (ranges == nullptr) return;

  SelectClusters(terrain_dag_, camera_.position, camera_.fov, screen_h_px_,
                 tau_px_, selected_clusters_);

  // Rewrite the draw ranges in place from the selected cut (bounds carried from
  // the DAG). The shared vertex/index buffers are untouched — the pass re-reads
  // ranges each frame and culls them per-frustum, so we never touch mesh.dirty.
  ranges->ranges.clear();
  ranges->ranges.reserve(selected_clusters_.size());
  sel_level_hist_.assign(terrain_dag_.level_count, 0);
  sel_tri_count_ = 0;
  for (uint32_t cidx : selected_clusters_) {
    const TerrainCluster& c = terrain_dag_.clusters[cidx];
    ranges->ranges.push_back(
        MeshDrawRange{c.first_index, c.index_count, c.bounds});
    sel_tri_count_ += c.index_count / 3;
    if (c.level < static_cast<int>(sel_level_hist_.size())) ++sel_level_hist_[c.level];
  }
  sel_cluster_count_ = static_cast<int>(selected_clusters_.size());

  // Log only when the cut size changes (not every frame) — one line per distinct
  // LOD state: exactly what the headless screenshot / smoke runs want, without
  // spamming an interactive fly-through.
  if (sel_cluster_count_ != last_logged_sel_count_) {
    spdlog::info(
        "cluster LOD cut: tau={:.2f}px screen_h={:.0f} cam_h={:.0f} -> {} "
        "clusters, {} tris",
        tau_px_, screen_h_px_, gamecam_.height, sel_cluster_count_,
        sel_tri_count_);
    last_logged_sel_count_ = sel_cluster_count_;
  }
}

void MapViewView::ApplyDebugTintMode() {
  if (cluster_entity_ == entt::null) return;
  auto* fmc = registry_.try_get<MaterialFactoryComponent>(cluster_entity_);
  if (fmc == nullptr) return;
  // Rewrite the per-draw override; render_textured_mesh transfers it each frame.
  fmc->params.uniform_overrides["debug_params"] =
      glm::vec4(static_cast<float>(debug_tint_mode_), 0.0f, 0.0f, 0.0f);
}

void MapViewView::BuildLegacyTerrain() {
  const DeferredMaterial terrain_mat = matlib_.TerrainBlend(terrain_arrays_);

  // One indexed kTerrainBlend chunk entity per N x N block region.
  const int blocks_x = cfg_.width / mapgen::kSamplesPerBlock;
  const int blocks_z = cfg_.height / mapgen::kSamplesPerBlock;
  for (int bz = 0; bz < blocks_z; bz += kChunkBlocks) {
    for (int bx = 0; bx < blocks_x; bx += kChunkBlocks) {
      TerrainMeshParams p;
      p.subdiv = kSubdiv;
      p.block_x0 = bx;
      p.block_z0 = bz;
      p.blocks_x = std::min(kChunkBlocks, blocks_x - bx);
      p.blocks_z = std::min(kChunkBlocks, blocks_z - bz);
      TerrainMesh chunk = BuildTerrainMesh(map_.heightmap, map_.biomes.pixel, p);
      if (chunk.vertex_count == 0) continue;

      const Aabb box = ComputeLocalAabbFromVertices(
          chunk.vertices, TerrainMesh::kFloatsPerVertex);

      const entt::entity e = registry_.create();
      auto& mesh = registry_.emplace<StaticTexturedMeshComponent>(e);
      mesh.vertices = std::move(chunk.vertices);
      mesh.indices = std::move(chunk.indices);
      mesh.vertex_count = chunk.vertex_count;
      mesh.dirty = true;
      mesh.geometry_type = GeometryType::kTerrainBlend;
      mesh.transform = glm::mat4(1.0f);  // vertices are absolute world coords

      MaterialFactoryComponent fmc;
      fmc.factory = terrain_mat.factory;
      fmc.pass_type = MaterialPassType::kDeferred;
      fmc.params = terrain_mat.params;
      fmc.config_hash = ComputeFactoryConfigHash(fmc);
      registry_.emplace<MaterialFactoryComponent>(e, std::move(fmc));
      registry_.emplace<StaticMeshAabbComponent>(e,
                                                 StaticMeshAabbComponent{box});
      legacy_entities_.push_back(e);
      ++chunk_count_;
    }
  }
}

void MapViewView::ApplyDaylight() {
  const DaylightState state = ComputeDaylight(daylight_cfg_, time_of_day_);
  ApplyDaylightEnvironment(state, daylight_cfg_, device_, queue_, sky_cube_,
                           scene_context_);
}

float MapViewView::SectionHeight(const mapgen::Block& b) const {
  if (b.section_id < 0 ||
      b.section_id >= static_cast<int>(map_.graph.nodes.size())) {
    return b.height;  // unsectioned block: fall back to its own height
  }
  return map_.graph.nodes[b.section_id].mean_height;
}

void MapViewView::RebuildVisibleGrid() {
  grid_.Clear();
  const mapgen::Field2D<mapgen::Block>& blocks = map_.blocks;
  if (blocks.width == 0 || blocks.height == 0 || !hover_valid_) return;

  const float b = static_cast<float>(mapgen::kBlockSizeM);
  // The grid is a flat plane per section, but the rendered terrain follows the
  // full-resolution heightmap — so the surface rises above the section mean by
  // the intra-section variation (cfg.variation_amp_m, up to ~0.3 m) plus the
  // block-median-vs-sample error. Lift by more than that or the plane sinks into
  // the terrain and the grid gets depth-occluded in patches. Still comfortably
  // under a terrace step (cfg.terrace_step_m, 1.2 m by default), so the grid
  // stays visually on its OWN terrace rather than floating up onto the next.
  const float lift_block = 0.8f;
  const float lift_section = 1.0f;
  const glm::vec3 block_color(0.55f, 0.55f, 0.6f);
  const glm::vec3 section_color(1.0f, 0.85f, 0.15f);
  // Subgrid: dimmer + thinner so the block structure still reads on top of
  // it. Mirrors what BuildTerrainMesh actually emits (kSubdiv cells per block
  // edge, each X-split into 4 triangles meeting at the cell centre), so this is
  // the mesh's real triangulation, not a decorative grid.
  const glm::vec3 subgrid_color(0.40f, 0.40f, 0.45f);
  const float subgrid_thickness = 0.6f;
  const float cell = b / static_cast<float>(kSubdiv);

  // Window of grid_radius_blocks_ around the point the mouse is over. Cost is
  // O(window^2), independent of the map size.
  const int cbx = static_cast<int>(std::floor(hover_point_.x / b));
  const int cbz = static_cast<int>(std::floor(hover_point_.z / b));
  const int r = std::max(1, grid_radius_blocks_);
  const int x_lo = std::max(0, cbx - r);
  const int x_hi = std::min(blocks.width - 1, cbx + r);
  const int z_lo = std::max(0, cbz - r);
  const int z_hi = std::min(blocks.height - 1, cbz + r);

  for (int bz = z_lo; bz <= z_hi; ++bz) {
    for (int bx = x_lo; bx <= x_hi; ++bx) {
      // Section height, not per-block height: a section is a terrace, so the
      // whole grid over it reads as ONE flat plane instead of stair-stepping.
      const float h = SectionHeight(blocks.at(bx, bz)) + lift_block;
      const float x0 = bx * b, x1 = (bx + 1) * b;
      const float z0 = bz * b, z1 = (bz + 1) * b;

      // Subgrid triangulation. Cell edges on a block boundary are skipped (cx/cz
      // == 0) -- the block pass below draws those, and doubling them up would
      // just overdraw two coincident AA quads.
      for (int cz = 0; cz < kSubdiv; ++cz) {
        for (int cx = 0; cx < kSubdiv; ++cx) {
          const float cx0 = x0 + cx * cell, cx1 = cx0 + cell;
          const float cz0 = z0 + cz * cell, cz1 = cz0 + cell;
          if (cz > 0) {
            grid_.AddLine({cx0, h, cz0}, {cx1, h, cz0}, subgrid_color,
                          subgrid_thickness);
          }
          if (cx > 0) {
            grid_.AddLine({cx0, h, cz0}, {cx0, h, cz1}, subgrid_color,
                          subgrid_thickness);
          }
          // X-split: each cell is 4 triangles fanning from its centre.
          const glm::vec3 c{0.5f * (cx0 + cx1), h, 0.5f * (cz0 + cz1)};
          grid_.AddLine({cx0, h, cz0}, c, subgrid_color, subgrid_thickness);
          grid_.AddLine({cx1, h, cz0}, c, subgrid_color, subgrid_thickness);
          grid_.AddLine({cx1, h, cz1}, c, subgrid_color, subgrid_thickness);
          grid_.AddLine({cx0, h, cz1}, c, subgrid_color, subgrid_thickness);
        }
      }

      // North + west edges (each interior edge drawn once); the window's own
      // south/east border is closed explicitly since the neighbor that would
      // otherwise draw it is outside the window.
      grid_.AddLine({x0, h, z0}, {x1, h, z0}, block_color, 1.0f);
      grid_.AddLine({x0, h, z0}, {x0, h, z1}, block_color, 1.0f);
      if (bx == x_hi) grid_.AddLine({x1, h, z0}, {x1, h, z1}, block_color, 1.0f);
      if (bz == z_hi) grid_.AddLine({x0, h, z1}, {x1, h, z1}, block_color, 1.0f);

      // Highlight ledges: edges to a neighbor in a different section (the
      // neighbor may be just outside the window — reading it is still in bounds).
      // Drawn on the UPPER terrace so the ledge sits on the step, not in it.
      const int sid = blocks.at(bx, bz).section_id;
      if (bx + 1 < blocks.width && blocks.at(bx + 1, bz).section_id != sid) {
        const float hh = std::max(SectionHeight(blocks.at(bx, bz)),
                                  SectionHeight(blocks.at(bx + 1, bz))) +
                         lift_section;
        grid_.AddLine({x1, hh, z0}, {x1, hh, z1}, section_color, 2.5f);
      }
      if (bz + 1 < blocks.height && blocks.at(bx, bz + 1).section_id != sid) {
        const float hh = std::max(SectionHeight(blocks.at(bx, bz)),
                                  SectionHeight(blocks.at(bx, bz + 1))) +
                         lift_section;
        grid_.AddLine({x0, hh, z1}, {x1, hh, z1}, section_color, 2.5f);
      }
    }
  }
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
      hover_valid_ = RaycastTerrain(map_.heightmap, ray, hover_point_);
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
      hover_valid_ = RaycastTerrain(map_.heightmap, ray, hover_point_);
      break;
    }
    default:
      break;
  }
}

void MapViewView::Update(float dt, const bool* keyboard_state) {
  if (keyboard_state != nullptr && ImGui::GetCurrentContext() != nullptr &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    glm::vec2 dir(0.0f);
    if (keyboard_state[SDL_SCANCODE_W] || keyboard_state[SDL_SCANCODE_UP]) dir.y -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_S] || keyboard_state[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    if (keyboard_state[SDL_SCANCODE_A] || keyboard_state[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_D] || keyboard_state[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
    if (dir.x != 0.0f || dir.y != 0.0f) {
      gamecam_.Pan(glm::normalize(dir) * gamecam_.pan_speed * dt);
    }
  }
  gamecam_.UpdateCamera(camera_);

  // Re-select the LOD cluster cut for the new camera and rewrite the draw
  // ranges. Cheap flat pass over the DAG; no buffer re-upload.
  UpdateClusterLod();

  // Rebuild the grid window around the hover point each frame (the camera may
  // have panned under a stationary cursor). No cache: the window is small
  // (independent of map size), so this is cheap.
  if (grid_visible_ && hover_valid_) {
    RebuildVisibleGrid();
    scene_context_.debug_lines = &grid_;
  } else {
    scene_context_.debug_lines = nullptr;
  }
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d m", cfg_.seed, cfg_.width, cfg_.height);
  ImGui::Text("chunks: %d   sections: %zu", chunk_count_,
              map_.graph.nodes.size());
  // A/B: cluster-LOD terrain (all leaf clusters, no LOD yet) vs the legacy
  // fixed-subdiv chunks. Flipping rebuilds the live terrain entities.
  if (ImGui::Checkbox("Cluster terrain", &use_cluster_terrain_)) {
    RebuildTerrain();
  }
  if (use_cluster_terrain_) {
    // LOD budget: screen-space error in pixels. Lower = finer/more clusters.
    // The rewrite happens next Update, so the numbers below refresh a frame
    // later (fine — they are diagnostics, not a synchronous readout).
    ImGui::SliderFloat("LOD tau (px)", &tau_px_, 0.25f, 16.0f, "%.2f");

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
          "  per-cluster color — that needs an engine change, deferred).\n"
          "LOD level: tint by the cluster's LOD level (hue wheel).");
    }

    // Compact stats block for the current LOD cut: totals then a one-line
    // per-level histogram of how many clusters each level contributes.
    ImGui::Text("cut: %d clusters   %llu tris", sel_cluster_count_,
                static_cast<unsigned long long>(sel_tri_count_));
    std::string hist;
    for (size_t L = 0; L < sel_level_hist_.size(); ++L) {
      if (sel_level_hist_[L] == 0) continue;
      if (!hist.empty()) hist += "  ";
      hist += "L" + std::to_string(L) + ":" + std::to_string(sel_level_hist_[L]);
    }
    ImGui::TextUnformatted(hist.empty() ? "  levels: (none selected)"
                                        : ("  levels  " + hist).c_str());
  }
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  if (hover_valid_) {
    const int bx = static_cast<int>(hover_point_.x / mapgen::kBlockSizeM);
    const int bz = static_cast<int>(hover_point_.z / mapgen::kBlockSizeM);
    int sid = -1;
    if (map_.blocks.in_bounds(bx, bz)) sid = map_.blocks.at(bx, bz).section_id;
    ImGui::Text("hover: (%.1f, %.1f, %.1f)  block %d,%d  section %d",
                hover_point_.x, hover_point_.y, hover_point_.z, bx, bz, sid);
  } else {
    ImGui::TextUnformatted("hover: (off terrain)");
  }
  ImGui::Checkbox("Grid (block + section)", &grid_visible_);
  ImGui::SliderInt("Grid radius (blocks)", &grid_radius_blocks_, 1, 30);
  // Scrub the sun. Re-bakes only on change (the bake is a CPU per-texel sky
  // eval + SH projection -- see ApplyDaylightEnvironment).
  if (ImGui::SliderFloat("Time of day", &time_of_day_, 0.0f, 1.0f, "%.3f")) {
    ApplyDaylight();
  }
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  // The LOD screen-space-error metric is in pixels, so it needs the viewport
  // height in pixels — exactly what OnResize carries (physical pixels windowed,
  // the capture height headless).
  if (height > 0) screen_h_px_ = static_cast<float>(height);
}

}  // namespace badlands
