#include "mapview/map_view_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // ComputeLocalAabbFromVertices
#include "engine/rendering/texture_loader.hpp"                   // CreateSolidColorArray
#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/biome_assign.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/config.hpp"
#include "mapgen/fields.hpp"
#include "mapgen/heightmap.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/voronoi.hpp"

namespace badlands {

namespace {
constexpr int kChunkBlocks = 16;  // N x N blocks per chunk (160 m)
constexpr int kSubdiv = 2;        // subgrid cells per block edge
constexpr const char* kBiomeManifestPath = "assets/materials/terrain_biomes.json";
}  // namespace

bool ResolveBiomePacks(const std::string& manifest_path,
                       std::vector<std::string>& out_pack_dirs) {
  std::ifstream file(manifest_path);
  if (!file) {
    spdlog::error("MapViewView: missing biome manifest '{}'", manifest_path);
    return false;
  }
  nlohmann::json manifest;
  try {
    file >> manifest;
  } catch (const nlohmann::json::exception& e) {
    spdlog::error("MapViewView: unparseable biome manifest '{}': {}",
                  manifest_path, e.what());
    return false;
  }

  // Layer index == Biome enum value, so resolve in enum order. Keyed by name so
  // a reordered/renamed entry fails loudly instead of silently mis-mapping.
  out_pack_dirs.clear();
  out_pack_dirs.reserve(mapgen::kBiomeCount);
  for (int i = 0; i < mapgen::kBiomeCount; ++i) {
    const std::string name(
        mapgen::biome_name(static_cast<mapgen::Biome>(i)));
    if (!manifest.contains(name) || !manifest[name].is_string()) {
      spdlog::error(
          "MapViewView: biome manifest '{}' has no pack for biome '{}'",
          manifest_path, name);
      return false;
    }
    out_pack_dirs.push_back(manifest[name].get<std::string>());
  }
  return true;
}

bool MapViewView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
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

  // Generate the map in-process.
  mapgen::MapgenConfig cfg;
  cfg.seed = seed_;
  cfg.width = resolution_;
  cfg.height = resolution_;
  mapgen::Fields fields;
  std::string err;
  if (!mapgen::evaluate_fields(cfg, "scripts/mapgen/fields.noiser", fields,
                               err)) {
    spdlog::error("MapViewView: {}", err);
    return false;
  }
  mapgen::Voronoi voronoi = mapgen::build_voronoi(cfg);
  mapgen::BiomeMap biomes = mapgen::assign_biomes(cfg, voronoi, fields);
  mapgen::Field2D<float> heightmap =
      mapgen::compose_heightmap(cfg, fields, biomes);
  map_size_m_ = static_cast<float>(resolution_) * mapgen::kMetersPerSample;

  // Blocks + sections drive the debug grid (kept for the per-frame windowed
  // rebuild in Update / RebuildVisibleGrid).
  block_grid_ = mapgen::reduce_to_blocks(heightmap, biomes.pixel,
                                         cfg.reduce_median);
  mapgen::SectionGraph graph = mapgen::extract_sections(
      block_grid_, cfg.section_step_m, cfg.min_section_blocks);
  section_count_ = static_cast<int>(graph.nodes.size());

  // One indexed kTerrainBlend chunk entity per N x N block region.
  const int blocks = resolution_ / mapgen::kSamplesPerBlock;
  for (int bz = 0; bz < blocks; bz += kChunkBlocks) {
    for (int bx = 0; bx < blocks; bx += kChunkBlocks) {
      TerrainMeshParams p;
      p.subdiv = kSubdiv;
      p.block_x0 = bx;
      p.block_z0 = bz;
      p.blocks_x = std::min(kChunkBlocks, blocks - bx);
      p.blocks_z = std::min(kChunkBlocks, blocks - bz);
      TerrainMesh chunk = BuildTerrainMesh(heightmap, biomes.pixel, p);
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
  spdlog::info("MapViewView: {}x{} map, {} chunks", resolution_, resolution_,
               chunk_count_);

  // Frame the fixed-angle camera on the map centre.
  gamecam_.focus = glm::vec3(map_size_m_ * 0.5f, 0.0f, map_size_m_ * 0.5f);
  gamecam_.pitch_deg = 55.0f;
  gamecam_.height = std::max(200.0f, map_size_m_ * 0.5f);
  gamecam_.UpdateCamera(camera_);

  // Build the initial grid window (Update rebuilds it as the camera pans; done
  // here too so the first/headless frame has it).
  RebuildVisibleGrid();
  scene_context_.debug_lines = &grid_;
  return true;
}

void MapViewView::RebuildVisibleGrid() {
  grid_.Clear();
  const mapgen::Field2D<mapgen::Block>& blocks = block_grid_;
  if (blocks.width == 0 || blocks.height == 0) return;

  const float b = static_cast<float>(mapgen::kBlockSizeM);
  const float lift_block = 0.3f;
  const float lift_section = 0.7f;
  const glm::vec3 block_color(0.55f, 0.55f, 0.6f);
  const glm::vec3 section_color(1.0f, 0.85f, 0.15f);

  // Window of kGridRadiusBlocks around the camera focus projected on the map
  // plane (XZ). Cost is O(window^2), independent of the map size.
  const int cbx = static_cast<int>(std::floor(gamecam_.focus.x / b));
  const int cbz = static_cast<int>(std::floor(gamecam_.focus.z / b));
  const int x_lo = std::max(0, cbx - kGridRadiusBlocks);
  const int x_hi = std::min(blocks.width - 1, cbx + kGridRadiusBlocks);
  const int z_lo = std::max(0, cbz - kGridRadiusBlocks);
  const int z_hi = std::min(blocks.height - 1, cbz + kGridRadiusBlocks);

  for (int bz = z_lo; bz <= z_hi; ++bz) {
    for (int bx = x_lo; bx <= x_hi; ++bx) {
      const float h = blocks.at(bx, bz).height + lift_block;
      const float x0 = bx * b, x1 = (bx + 1) * b;
      const float z0 = bz * b, z1 = (bz + 1) * b;
      // North + west edges (each interior edge drawn once); the window's own
      // south/east border is closed explicitly since the neighbor that would
      // otherwise draw it is outside the window.
      grid_.AddLine({x0, h, z0}, {x1, h, z0}, block_color, 1.0f);
      grid_.AddLine({x0, h, z0}, {x0, h, z1}, block_color, 1.0f);
      if (bx == x_hi) grid_.AddLine({x1, h, z0}, {x1, h, z1}, block_color, 1.0f);
      if (bz == z_hi) grid_.AddLine({x0, h, z1}, {x1, h, z1}, block_color, 1.0f);

      // Highlight ledges: edges to a neighbor in a different section (the
      // neighbor may be just outside the window — reading it is still in bounds).
      const int sid = blocks.at(bx, bz).section_id;
      if (bx + 1 < blocks.width &&
          blocks.at(bx + 1, bz).section_id != sid) {
        const float hh =
            std::max(blocks.at(bx, bz).height, blocks.at(bx + 1, bz).height) +
            lift_section;
        grid_.AddLine({x1, hh, z0}, {x1, hh, z1}, section_color, 2.5f);
      }
      if (bz + 1 < blocks.height &&
          blocks.at(bx, bz + 1).section_id != sid) {
        const float hh =
            std::max(blocks.at(bx, bz).height, blocks.at(bx, bz + 1).height) +
            lift_section;
        grid_.AddLine({x0, hh, z1}, {x1, hh, z1}, section_color, 2.5f);
      }
    }
  }
}

void MapViewView::HandleEvent(const SDL_Event& /*event*/, int /*w*/,
                              int /*h*/) {}

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

  // Rebuild the grid window around the (possibly panned) camera each frame. No
  // cache: the window is small (independent of map size), so this is cheap.
  if (grid_visible_) {
    RebuildVisibleGrid();
    scene_context_.debug_lines = &grid_;
  } else {
    scene_context_.debug_lines = nullptr;
  }
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d m", seed_, static_cast<int>(map_size_m_),
              static_cast<int>(map_size_m_));
  ImGui::Text("chunks: %d   sections: %d", chunk_count_, section_count_);
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  ImGui::Checkbox("Grid (block + section)", &grid_visible_);
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
