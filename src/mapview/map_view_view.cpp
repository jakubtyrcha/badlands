#include "mapview/map_view_view.hpp"

#include <algorithm>
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
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // ComputeLocalAabbFromVertices
#include "engine/rendering/scene_renderer.hpp"  // GetFogSimulation / MutableFogConfig
#include "engine/ui/editor_ui.hpp"
#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/fog_generator.hpp"
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
  scene_renderer_ = ctx.scene_renderer;  // shared debug/fog selectors need it

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  // Start at noon, paused (an inspector holds still until you play/scrub).
  sim_clock_.speed = 0.0f;
  sim_clock_.SeekTimeOfDay(0.5f);
  ApplyDaylight();
  scene_context_.registry = &registry_;
  // Fog renders when enabled; sources come from the biome generator (a later
  // phase). Enabled here so the fog editor is exercisable.
  if (scene_renderer_) scene_renderer_->MutableFogConfig().enabled = true;

  // One PBR pack per biome, layer index = Biome enum value. The mapping is data
  // (assets/materials/terrain_biomes.json); the engine only sees "N packs".
  std::vector<std::string> pack_dirs;
  if (!ResolveBiomePacks(kBiomeManifestPath, pack_dirs)) return false;
  terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  if (!matlib_.ok()) {
    spdlog::error("MapViewView: terrain material packs failed to load");
    return false;
  }
  const DeferredMaterial terrain_mat = matlib_.TerrainBlend(terrain_arrays_);

  // Build the map in-process — the same pipeline --preview-image-only dumps, so the
  // rendered terrain and the preview PNGs can never disagree. An authored map loads
  // its terrain from images instead of generating it; both share everything after.
  std::string err;
  const bool ok = cfg_.map_dir.empty()
                      ? mapgen::run_pipeline(cfg_, "scripts/mapgen/fields.noiser",
                                             map_, err)
                      : mapgen::run_authored_pipeline(cfg_, cfg_.map_dir, map_, err);
  if (!ok) {
    spdlog::error("MapViewView: {}", err);
    return false;
  }
  map_size_m_ = static_cast<float>(cfg_.width) * mapgen::kMetersPerSample;

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
      ++chunk_count_;
    }
  }
  spdlog::info("MapViewView: {}x{} map, {} chunks, {} sections", cfg_.width,
               cfg_.height, chunk_count_, map_.graph.nodes.size());

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
  gamecam_.UpdateCamera(camera_);

  // Derive world-static fog emitters from the biome map (forest -> flat elliptical
  // fog, swamp -> granular noise fog) and push them to the renderer's fog sim.
  fog_emitters_ = mapgen::GenerateBiomeFog(map_.biomes.pixel, map_.heightmap,
                                           cfg_.seed);
  spdlog::info("MapViewView: {} biome fog emitters", fog_emitters_.size());
  SetFogSources();

  // The grid follows the mouse, so there is nothing to draw until the cursor is
  // over the terrain (Update wires debug_lines once hover_valid_).
  return true;
}

std::vector<fog::Emitter> MapViewView::BuildEdgeEmitters() const {
  std::vector<fog::Emitter> out;
  if (!edge_fog_.enabled) return out;

  // Unproject the four screen corners to the ground plane. Any screen size works
  // (the ray depends only on the normalized position), so use a unit square.
  const glm::vec2 sz(1.0f, 1.0f);
  const glm::vec2 px[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};  // TL, TR, BR, BL
  glm::vec3 corner[4];
  for (int i = 0; i < 4; ++i) {
    if (!IntersectGroundPlane(ScreenPointToRay(camera_, px[i], sz), 0.0f, corner[i]))
      return out;  // an edge points at/above the horizon: no wall this frame
  }

  const float band = edge_fog_.band_m;
  for (int i = 0; i < 4; ++i) {
    const glm::vec2 a(corner[i].x, corner[i].z);
    const glm::vec2 b(corner[(i + 1) % 4].x, corner[(i + 1) % 4].z);
    glm::vec2 dir = b - a;
    const float len = glm::length(dir);
    if (len < 1e-3f) continue;
    dir /= len;

    fog::Emitter e;
    // OBB centred ON the edge line: full density at the edge (local perp 0),
    // ramping to 0 by `band` inward (the outer half is off-screen). Extended past
    // the corners so adjacent walls overlap (max-combined, no corner gap).
    e.center = 0.5f * (a + b);
    e.rotation = std::atan2(dir.y, dir.x);
    e.half_extent = {0.5f * len + band, band};  // along edge (padded), perp band
    e.shape = fog::EmitterShape::Obb;
    e.type = fog::EmitterType::Disc;  // flat milk-white
    e.base_y = 0.0f;
    e.height = edge_fog_.height_m;
    e.magnitude = edge_fog_.magnitude;
    // Ramp only the outer `ramp_m` of the perpendicular reach; along-edge stays
    // full because its normalized distance never exceeds ~0.5 across the view.
    e.radial_falloff = std::clamp(edge_fog_.ramp_m / std::max(band, 1e-3f), 0.0f, 1.0f);
    e.vertical_falloff = 0.3f;
    out.push_back(e);
  }
  return out;
}

void MapViewView::SetFogSources() {
  if (scene_renderer_ == nullptr) return;
  // Biome emitters (edited/picked) plus the transient edge-fog wall, appended
  // after so they never shift the biome indices the editor addresses.
  std::vector<fog::Emitter> all = fog_emitters_;
  const std::vector<fog::Emitter> edges = BuildEdgeEmitters();
  all.insert(all.end(), edges.begin(), edges.end());

  FogSimParams p;
  // Include the camera's edge band, which can reach past the map, so the wall
  // still buckets into the broadphase when the camera looks off the map edge.
  p.map_min = {-256.0f, -256.0f};
  p.map_max = {static_cast<float>(cfg_.width) * mapgen::kMetersPerSample + 256.0f,
               static_cast<float>(cfg_.height) * mapgen::kMetersPerSample + 256.0f};
  p.bp_cell_size = 32.0f;
  scene_renderer_->GetFogSimulation().SetSources(all, p);
}

void MapViewView::ApplyDaylight() {
  const DaylightState state =
      ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());
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
  // NB: does NOT clear grid_ -- the caller clears it once and may add other
  // overlays (the selected fog emitter's OBB) to the same buffer.
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
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (event.button.button != SDL_BUTTON_LEFT) break;
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.button.windowID, screen)) break;
      const Ray ray = ScreenPointToRay(
          camera_, glm::vec2(event.button.x, event.button.y), screen);
      glm::vec3 hit;
      if (RaycastTerrain(map_.heightmap, ray, hit)) selected_emitter_ = PickEmitter(hit);
      break;
    }
    default:
      break;
  }
}

int MapViewView::PickEmitter(const glm::vec3& world) const {
  // Which emitter footprint contains `world` (XZ), in its local frame. Nearest
  // centre wins on overlap. -1 = none (deselect).
  int best = -1;
  float best_d2 = 1e30f;
  for (size_t i = 0; i < fog_emitters_.size(); ++i) {
    const fog::Emitter& e = fog_emitters_[i];
    const glm::vec2 d = glm::vec2(world.x, world.z) - e.center;
    const float cs = std::cos(e.rotation), sn = std::sin(e.rotation);
    const glm::vec2 local(d.x * cs + d.y * sn, -d.x * sn + d.y * cs);  // rotate by -yaw
    const glm::vec2 nd(local.x / std::max(e.half_extent.x, 1e-4f),
                       local.y / std::max(e.half_extent.y, 1e-4f));
    const bool inside = (e.shape == fog::EmitterShape::Obb)
                            ? (std::abs(nd.x) <= 1.0f && std::abs(nd.y) <= 1.0f)
                            : (nd.x * nd.x + nd.y * nd.y <= 1.0f);  // disc/ellipse
    if (!inside) continue;
    const float d2 = d.x * d.x + d.y * d.y;
    if (d2 < best_d2) { best_d2 = d2; best = static_cast<int>(i); }
  }
  return best;
}

void MapViewView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Advance the shared clock; when it's running, move the sun and animate the
  // fog by the same sim delta (paused => both hold). The daylight re-bake is
  // throttled implicitly: ApplyDaylight is only called while time actually moves.
  const double sim_dt = sim_clock_.Advance(dt);
  if (sim_dt > 0.0) {
    ApplyDaylight();
    if (scene_renderer_) {
      scene_renderer_->GetFogSimulation().AddTime(static_cast<float>(sim_dt));
    }
  }

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

  // The edge-fog wall is placed from the current frustum, so refresh the fog
  // sources each frame while it's on (cheap: a few hundred emitters + broadphase).
  if (edge_fog_.enabled) SetFogSources();

  // Rebuild the debug-line overlay each frame into one buffer: the hover grid plus
  // the selected fog emitter's OBB. No cache: both are small (independent of map
  // size), so this is cheap.
  grid_.Clear();
  if (grid_visible_ && hover_valid_) RebuildVisibleGrid();
  if (selected_emitter_ >= 0 &&
      selected_emitter_ < static_cast<int>(fog_emitters_.size())) {
    const fog::Emitter& e = fog_emitters_[selected_emitter_];
    grid_.AddOrientedBox(e.center, e.rotation, e.half_extent, e.base_y,
                         e.base_y + e.height, glm::vec3(1.0f, 0.25f, 0.9f), 2.5f);
  }
  scene_context_.debug_lines = grid_.empty() ? nullptr : &grid_;
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d m", cfg_.seed, cfg_.width, cfg_.height);
  ImGui::Text("chunks: %d   sections: %zu", chunk_count_,
              map_.graph.nodes.size());
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
    EditorUI::DrawFogEditor(*scene_renderer_);
    if (ImGui::CollapsingHeader("Debug Views")) {
      EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
      EditorUI::DrawShadowDebugSelector(*scene_renderer_);
    }
  }
  EditorUI::DrawStats(dt_);
  ImGui::End();

  DrawFogEmitterEditor();
}

void MapViewView::DrawFogEmitterEditor() {
  ImGui::Begin("Fog Emitters");
  ImGui::Text("%zu emitters   (click the terrain to select)", fog_emitters_.size());
  if (ImGui::Button("Regenerate from biomes")) {
    fog_emitters_ =
        mapgen::GenerateBiomeFog(map_.biomes.pixel, map_.heightmap, cfg_.seed);
    selected_emitter_ = -1;
    SetFogSources();
  }
  ImGui::SameLine();
  if (ImGui::Button("Deselect")) selected_emitter_ = -1;

  // Screen-edge fog wall.
  if (ImGui::CollapsingHeader("Edge fog", ImGuiTreeNodeFlags_DefaultOpen)) {
    bool changed = ImGui::Checkbox("Enabled##edge", &edge_fog_.enabled);
    changed |= ImGui::SliderFloat("Band (m)", &edge_fog_.band_m, 4.0f, 128.0f);
    changed |= ImGui::SliderFloat("Ramp (m)", &edge_fog_.ramp_m, 0.0f, 32.0f);
    changed |= ImGui::SliderFloat("Magnitude##edge", &edge_fog_.magnitude, 0.0f, 0.5f, "%.3f");
    changed |= ImGui::SliderFloat("Height##edge", &edge_fog_.height_m, 1.0f, 128.0f);
    if (changed) SetFogSources();  // reflect immediately (also removes the wall when off)
  }

  if (selected_emitter_ < 0 ||
      selected_emitter_ >= static_cast<int>(fog_emitters_.size())) {
    ImGui::TextUnformatted("No emitter selected.");
    ImGui::End();
    return;
  }

  fog::Emitter& e = fog_emitters_[selected_emitter_];
  ImGui::SeparatorText("Selected emitter");
  ImGui::Text("#%d  center (%.0f, %.0f)", selected_emitter_, e.center.x, e.center.y);

  bool changed = false;
  const char* kShapes[] = {"Disc", "OBB", "Ellipse"};
  int shape = static_cast<int>(e.shape);
  if (ImGui::Combo("Shape", &shape, kShapes, 3)) {
    e.shape = static_cast<fog::EmitterShape>(shape);
    changed = true;
  }
  const char* kTypes[] = {"Flat (Disc)", "Noise"};
  int type = static_cast<int>(e.type);
  if (ImGui::Combo("Type", &type, kTypes, 2)) {
    e.type = static_cast<fog::EmitterType>(type);
    changed = true;
  }
  changed |= ImGui::DragFloat2("Half extent", &e.half_extent.x, 0.1f, 0.5f, 200.0f);
  changed |= ImGui::SliderAngle("Rotation", &e.rotation);
  changed |= ImGui::DragFloat("Base Y", &e.base_y, 0.1f);
  changed |= ImGui::SliderFloat("Height", &e.height, 1.0f, 128.0f);
  changed |= ImGui::SliderFloat("Magnitude", &e.magnitude, 0.0f, 0.5f, "%.3f");
  changed |= ImGui::SliderFloat("Radial falloff", &e.radial_falloff, 0.0f, 1.0f);
  changed |= ImGui::SliderFloat("Vertical falloff", &e.vertical_falloff, 0.0f, 1.0f);
  if (e.type == fog::EmitterType::Noise) {
    changed |= ImGui::SliderFloat("Noise freq", &e.noise_freq, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Noise contrast", &e.noise_contrast, 0.1f, 4.0f);
    changed |= ImGui::DragFloat3("Scroll", &e.scroll.x, 0.05f);
  }
  if (changed) SetFogSources();  // re-upload the whole set
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
