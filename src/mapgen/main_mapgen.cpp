// badlands_mapgen: offline CPU map generator (milestone 1).
//
// Pure-CPU tool — no window/GPU/AppView. noiser (VM, release) evaluates the
// continuous noise fields in parallel; plain C++ owns the stateful passes
// (voronoi, biome assignment, height composition, section extraction) and all
// PNG/JSON output. See docs/…/1-decouple-the-terrain plan.
//
// Usage: badlands_mapgen [--seed N] [--resolution WxH] [--out DIR] [--config F]
// Run from the repo root (scripts/mapgen/fields.noiser resolves relative to cwd).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "mapgen/biome_assign.hpp"
#include "mapgen/config.hpp"
#include "mapgen/fields.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/heightmap.hpp"
#include "mapgen/outputs.hpp"
#include "mapgen/sections.hpp"
#include "mapgen/voronoi.hpp"

namespace {

using badlands::mapgen::MapgenConfig;

std::optional<std::pair<int, int>> parse_resolution(const std::string& s) {
  auto x = s.find('x');
  if (x == std::string::npos) return std::nullopt;
  try {
    int w = std::stoi(s.substr(0, x));
    int h = std::stoi(s.substr(x + 1));
    if (w <= 0 || h <= 0) return std::nullopt;
    return std::make_pair(w, h);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::optional<uint32_t> seed_override;
  std::optional<std::pair<int, int>> res_override;
  std::optional<std::string> out_override;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "mapgen: %s needs an argument\n", flag);
      return std::nullopt;
    };
    if (a == "--config") {
      if (auto v = next("--config")) config_path = *v; else return 2;
    } else if (a == "--seed") {
      auto v = next("--seed");
      if (!v) return 2;
      try {
        seed_override = static_cast<uint32_t>(std::stoul(*v));
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapgen: bad --seed '%s' (want a number)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--resolution") {
      auto v = next("--resolution");
      if (!v) return 2;
      res_override = parse_resolution(*v);
      if (!res_override) {
        std::fprintf(stderr, "mapgen: bad --resolution '%s' (want WxH)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--out") {
      if (auto v = next("--out")) out_override = *v; else return 2;
    } else {
      std::fprintf(stderr, "mapgen: unknown arg '%s'\n", a.c_str());
      return 2;
    }
  }

  MapgenConfig cfg = badlands::mapgen::load_config(config_path);
  if (seed_override) cfg.seed = *seed_override;
  if (res_override) {
    cfg.width = res_override->first;
    cfg.height = res_override->second;
  }
  if (out_override) cfg.out_dir = *out_override;

  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "mapgen: cannot create out dir '%s': %s\n",
                 cfg.out_dir.c_str(), ec.message().c_str());
    return 1;
  }

  if (cfg.width % badlands::mapgen::kSamplesPerBlock != 0 ||
      cfg.height % badlands::mapgen::kSamplesPerBlock != 0) {
    std::fprintf(stderr,
                 "mapgen: resolution %dx%d is not a multiple of %d; remainder "
                 "samples are dropped from the block grid\n",
                 cfg.width, cfg.height, badlands::mapgen::kSamplesPerBlock);
  }

  std::printf("mapgen: %dx%d seed=%u -> %s\n", cfg.width, cfg.height, cfg.seed,
              cfg.out_dir.c_str());

  badlands::mapgen::Fields fields;
  std::string err;
  if (!badlands::mapgen::evaluate_fields(cfg, "scripts/mapgen/fields.noiser",
                                         fields, err)) {
    std::fprintf(stderr, "mapgen: %s\n", err.c_str());
    return 1;
  }

  badlands::mapgen::write_gray_png(fields.elevation, cfg.out_dir + "/elevation.png");
  badlands::mapgen::write_gray_png(fields.moisture, cfg.out_dir + "/moisture.png");
  badlands::mapgen::write_gray_png(fields.ridged, cfg.out_dir + "/ridged.png");
  badlands::mapgen::write_gray_png(fields.fine, cfg.out_dir + "/fine.png");

  badlands::mapgen::Voronoi voronoi = badlands::mapgen::build_voronoi(cfg);
  badlands::mapgen::write_hashed_png(voronoi.cell, cfg.out_dir + "/voronoi.png");
  std::printf("mapgen: %d voronoi cells (%dx%d grid)\n", voronoi.cell_count(),
              voronoi.cols, voronoi.rows);

  badlands::mapgen::BiomeMap biomes =
      badlands::mapgen::assign_biomes(cfg, voronoi, fields);
  badlands::mapgen::write_biome_png(biomes.pixel, cfg.out_dir + "/biome.png");

  badlands::mapgen::Field2D<float> heightmap =
      badlands::mapgen::compose_heightmap(cfg, fields, biomes);
  badlands::mapgen::write_gray_png(heightmap, cfg.out_dir + "/heightmap.png");

  badlands::mapgen::Field2D<badlands::mapgen::Block> blocks =
      badlands::mapgen::reduce_to_blocks(heightmap, biomes.pixel,
                                         cfg.reduce_median);
  badlands::mapgen::SectionGraph graph = badlands::mapgen::extract_sections(
      blocks, cfg.section_step_m, cfg.min_section_blocks);
  badlands::mapgen::write_sections_png(blocks, cfg.out_dir + "/sections.png");
  badlands::mapgen::write_section_graph_json(graph,
                                             cfg.out_dir + "/sections.json");
  std::printf("mapgen: %zu sections, %zu ledges (%dx%d blocks)\n",
              graph.nodes.size(), graph.edges.size(), blocks.width,
              blocks.height);

  std::printf("mapgen: done (%s)\n", cfg.out_dir.c_str());
  return 0;
}
