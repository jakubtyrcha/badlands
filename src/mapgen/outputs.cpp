#include "mapgen/outputs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>

#include "core/util/cpu_image.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/mapgen_constants.hpp"

namespace badlands::mapgen {

namespace {

// Map an id to a distinct, reasonably-spread RGB via integer hashing.
badlands::CpuImage::Color id_color(int id) {
  uint32_t h = static_cast<uint32_t>(id) * 2654435761u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return {static_cast<uint8_t>(h & 0xff),
          static_cast<uint8_t>((h >> 8) & 0xff),
          static_cast<uint8_t>((h >> 16) & 0xff), 255};
}

}  // namespace

void write_preview_images(const MapgenConfig& cfg, const MapArtifacts& a) {
  const std::string& d = cfg.out_dir;
  // Raw noise fields (diagnostics).
  write_gray_png(a.fields.elevation, d + "/elevation.png");
  write_gray_png(a.fields.moisture, d + "/moisture.png");
  write_gray_png(a.fields.ridged, d + "/ridged.png");
  write_gray_png(a.fields.fine, d + "/fine.png");
  // Structure + semantics.
  write_hashed_png(a.voronoi.cell, d + "/voronoi.png");
  write_biome_png(a.biomes.pixel, d + "/biome.png");
  write_gray_png(a.heightmap, d + "/heightmap.png");
  write_sections_png(a.blocks, d + "/sections.png");
  write_section_graph_json(a.graph, d + "/sections.json");
}

void write_gray_png(const Field2D<float>& field, const std::string& path,
                    bool normalize) {
  float lo = 0.0f;
  float hi = 1.0f;
  if (normalize) {
    lo = std::numeric_limits<float>::max();
    hi = std::numeric_limits<float>::lowest();
    for (float v : field.data) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (!(hi > lo)) {  // constant field (or empty) — avoid divide-by-zero
      lo = 0.0f;
      hi = 1.0f;
    }
  }
  const float span = hi - lo;

  badlands::CpuImage img(static_cast<uint32_t>(field.width),
                         static_cast<uint32_t>(field.height),
                         wgpu::TextureFormat::R8Unorm);
  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      float t = (field.at(x, y) - lo) / span;
      t = std::clamp(t, 0.0f, 1.0f);
      img.SetPixelF32(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                      {t, t, t, 1.0f});
    }
  }
  img.WritePng(path);
}

void write_hashed_png(const Field2D<int>& field, const std::string& path) {
  badlands::CpuImage img(static_cast<uint32_t>(field.width),
                         static_cast<uint32_t>(field.height),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      img.SetPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                   id_color(field.at(x, y)));
    }
  }
  img.WritePng(path);
}

void write_biome_png(const Field2D<uint8_t>& biome, const std::string& path) {
  badlands::CpuImage img(static_cast<uint32_t>(biome.width),
                         static_cast<uint32_t>(biome.height),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int y = 0; y < biome.height; ++y) {
    for (int x = 0; x < biome.width; ++x) {
      const Rgb c = biome_color(static_cast<Biome>(biome.at(x, y)));
      img.SetPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                   {c.r, c.g, c.b, 255});
    }
  }
  img.WritePng(path);
}

void write_sections_png(const Field2D<Block>& blocks,
                        const std::string& path) {
  const int W = blocks.width * kSamplesPerBlock;
  const int H = blocks.height * kSamplesPerBlock;
  badlands::CpuImage img(static_cast<uint32_t>(W), static_cast<uint32_t>(H),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int py = 0; py < H; ++py) {
    for (int px = 0; px < W; ++px) {
      const int bx = px / kSamplesPerBlock;
      const int by = py / kSamplesPerBlock;
      const int sid = blocks.at(bx, by).section_id;
      badlands::CpuImage::Color c = id_color(sid);

      // Draw a dark 1px ledge on the block's left/top edge when the neighbor
      // block belongs to a different section.
      const bool left_edge = (px % kSamplesPerBlock) == 0 && bx > 0 &&
                             blocks.at(bx - 1, by).section_id != sid;
      const bool top_edge = (py % kSamplesPerBlock) == 0 && by > 0 &&
                            blocks.at(bx, by - 1).section_id != sid;
      if (left_edge || top_edge) {
        c = {static_cast<uint8_t>(c.r / 4), static_cast<uint8_t>(c.g / 4),
             static_cast<uint8_t>(c.b / 4), 255};
      }
      img.SetPixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py), c);
    }
  }
  img.WritePng(path);
}

void write_section_graph_json(const SectionGraph& graph,
                              const std::string& path) {
  nlohmann::json j;
  j["block_size_m"] = kBlockSizeM;
  j["section_count"] = graph.nodes.size();
  auto& nodes = j["nodes"] = nlohmann::json::array();
  for (const auto& n : graph.nodes) {
    nodes.push_back({{"id", n.id},
                     {"biome", std::string(biome_name(n.biome))},
                     {"block_count", n.block_count},
                     {"mean_height", n.mean_height},
                     {"centroid_m", {n.centroid_m.x, n.centroid_m.y}}});
  }
  auto& edges = j["edges"] = nlohmann::json::array();
  for (const auto& e : graph.edges) {
    edges.push_back({{"a", e.a},
                     {"b", e.b},
                     {"height_step", e.height_step},
                     {"border_len", e.border_len}});
  }
  std::ofstream f(path);
  f << j.dump(2) << "\n";
}

}  // namespace badlands::mapgen
