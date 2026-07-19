// badlands_mapview: the map tool. Two modes, one pipeline.
//
//   --preview-image-only   run the generator and dump the debug rasters +
//                          section graph into --out, then exit. Pure CPU: no
//                          window, no GPU (SdlViewerApp is never constructed).
//   (default)              generate the map and render it as the in-game
//                          terrain (chunked, biome-blended) with a fixed-angle
//                          camera and a mouse-hover block/section grid.
//
// Run from the repo root (scripts/mapgen/fields.noiser, shaders/ and assets/
// resolve relative to cwd).
//
// Usage: badlands_mapview [--config F] [--seed N] [--resolution WxH] [--out DIR]
//                         [--preview-image-only] [--legacy-terrain]
//                         [--screenshot out.png] [--record dir/]
//
//   --legacy-terrain  render the fixed-subdiv chunk terrain instead of the
//                     default cluster-LOD terrain (the A/B baseline; also
//                     toggleable at runtime via the "Cluster terrain" checkbox).
//   --camera-height H starting camera height in metres (headless framing: a
//                     small H for a near shot, a large one for a far shot).
//   --lod-tau T       screen-space-error budget in pixels for cluster LOD
//                     (default 1.5; higher = coarser / fewer draws).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/app/sdl_viewer_app.hpp"
#include "mapgen/config.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/outputs.hpp"
#include "mapgen/pipeline.hpp"
#include "mapview/map_view_view.hpp"

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

// Flags owned by the app layer (SdlViewerApp::Run parses these out of the raw
// argv itself). We must skip them + their value rather than reject them as
// unknown, or --screenshot/--record would stop working here.
bool is_app_flag_with_value(const std::string& a) {
  return a == "--screenshot" || a == "--record";
}

// Runs the generator and dumps the rasters. Returns a process exit code.
int RunPreviewOnly(const MapgenConfig& cfg) {
  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "mapview: cannot create out dir '%s': %s\n",
                 cfg.out_dir.c_str(), ec.message().c_str());
    return 1;
  }

  std::printf("mapview: %dx%d seed=%u -> %s\n", cfg.width, cfg.height, cfg.seed,
              cfg.out_dir.c_str());

  badlands::mapgen::MapArtifacts artifacts;
  std::string err;
  if (!badlands::mapgen::run_pipeline(cfg, "scripts/mapgen/fields.noiser",
                                      artifacts, err)) {
    std::fprintf(stderr, "mapview: %s\n", err.c_str());
    return 1;
  }
  badlands::mapgen::write_preview_images(cfg, artifacts);

  std::printf("mapview: %d voronoi cells, %zu sections, %zu ledges (%dx%d "
              "blocks)\n",
              artifacts.voronoi.cell_count(), artifacts.graph.nodes.size(),
              artifacts.graph.edges.size(), artifacts.blocks.width,
              artifacts.blocks.height);
  std::printf("mapview: done (%s)\n", cfg.out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::optional<uint32_t> seed_override;
  std::optional<std::pair<int, int>> res_override;
  std::optional<std::string> out_override;
  bool preview_only = false;
  bool use_cluster_terrain = true;
  float camera_height = 0.0f;  // 0 = keep the default framing
  float lod_tau = 1.5f;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "mapview: %s needs an argument\n", flag);
      return std::nullopt;
    };
    if (a == "--preview-image-only") {
      preview_only = true;
    } else if (a == "--legacy-terrain") {
      use_cluster_terrain = false;
    } else if (a == "--config") {
      if (auto v = next("--config")) config_path = *v; else return 2;
    } else if (a == "--seed") {
      auto v = next("--seed");
      if (!v) return 2;
      try {
        seed_override = static_cast<uint32_t>(std::stoul(*v));
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad --seed '%s' (want a number)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--resolution") {
      auto v = next("--resolution");
      if (!v) return 2;
      res_override = parse_resolution(*v);
      if (!res_override) {
        std::fprintf(stderr, "mapview: bad --resolution '%s' (want WxH)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--out") {
      if (auto v = next("--out")) out_override = *v; else return 2;
    } else if (a == "--camera-height") {
      auto v = next("--camera-height");
      if (!v) return 2;
      try {
        camera_height = std::stof(*v);
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad --camera-height '%s' (want metres)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--lod-tau") {
      auto v = next("--lod-tau");
      if (!v) return 2;
      try {
        lod_tau = std::stof(*v);
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad --lod-tau '%s' (want pixels)\n",
                     v->c_str());
        return 2;
      }
    } else if (is_app_flag_with_value(a)) {
      if (!next(a.c_str())) return 2;  // consume the value; SdlViewerApp reads it
    } else {
      std::fprintf(stderr, "mapview: unknown arg '%s'\n", a.c_str());
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

  if (cfg.width % badlands::mapgen::kSamplesPerBlock != 0 ||
      cfg.height % badlands::mapgen::kSamplesPerBlock != 0) {
    std::fprintf(stderr,
                 "mapview: resolution %dx%d is not a multiple of %d; remainder "
                 "samples are dropped from the block grid\n",
                 cfg.width, cfg.height, badlands::mapgen::kSamplesPerBlock);
  }

  if (preview_only) return RunPreviewOnly(cfg);

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [cfg, use_cluster_terrain, camera_height, lod_tau](
                     const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(
                       cfg, use_cluster_terrain, camera_height, lod_tau);
                 });
}
