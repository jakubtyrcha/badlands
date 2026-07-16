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
#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/pipeline.hpp"
#include "mapview/biome_manifest.hpp"  // ResolveBiomePacks

namespace badlands {

namespace {
constexpr int kChunkBlocks = 16;  // N x N blocks per chunk (160 m)
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
  const DeferredMaterial terrain_mat = matlib_.TerrainBlend(terrain_arrays_);

  // Generate the map in-process — the same pipeline --preview-image-only dumps,
  // so the rendered terrain and the preview PNGs can never disagree.
  std::string err;
  if (!mapgen::run_pipeline(cfg_, "scripts/mapgen/fields.noiser", map_, err)) {
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

  // The grid follows the mouse, so there is nothing to draw until the cursor is
  // over the terrain (Update wires debug_lines once hover_valid_).
  return true;
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
  // Subgrid: dimmer + thinner so the 10 m block structure still reads on top of
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
}

}  // namespace badlands
