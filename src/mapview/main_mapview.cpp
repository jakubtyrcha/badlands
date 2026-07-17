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
//                         [--preview-image-only] [--screenshot out.png]
//                         [--record dir/]

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

// Builds the map and dumps the rasters. Returns a process exit code.
int RunPreviewOnly(MapgenConfig& cfg) {
  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "mapview: cannot create out dir '%s': %s\n",
                 cfg.out_dir.c_str(), ec.message().c_str());
    return 1;
  }

  const bool authored = !cfg.map_dir.empty();
  badlands::mapgen::MapArtifacts artifacts;
  std::string err;
  // An authored map takes its size from the asset, so print after the load, not
  // before it -- cfg.width/height are not yet meaningful here.
  const bool ok =
      authored ? badlands::mapgen::run_authored_pipeline(cfg, cfg.map_dir,
                                                         artifacts, err)
               : badlands::mapgen::run_pipeline(cfg, "scripts/mapgen/fields.noiser",
                                                artifacts, err);
  if (!ok) {
    std::fprintf(stderr, "mapview: %s\n", err.c_str());
    return 1;
  }
  if (authored) {
    std::printf("mapview: %dx%d authored (%s) -> %s\n", cfg.width, cfg.height,
                cfg.map_dir.c_str(), cfg.out_dir.c_str());
  } else {
    std::printf("mapview: %dx%d seed=%u -> %s\n", cfg.width, cfg.height, cfg.seed,
                cfg.out_dir.c_str());
  }
  badlands::mapgen::write_preview_images(cfg, artifacts);

  // An authored map has no voronoi cells -- reporting "0 cells" would read as a
  // failure rather than as "that stage never ran".
  if (authored) {
    std::printf("mapview: %zu sections, %zu ledges (%dx%d blocks)\n",
                artifacts.graph.nodes.size(), artifacts.graph.edges.size(),
                artifacts.blocks.width, artifacts.blocks.height);
  } else {
    std::printf("mapview: %d voronoi cells, %zu sections, %zu ledges (%dx%d "
                "blocks)\n",
                artifacts.voronoi.cell_count(), artifacts.graph.nodes.size(),
                artifacts.graph.edges.size(), artifacts.blocks.width,
                artifacts.blocks.height);
  }
  std::printf("mapview: done (%s)\n", cfg.out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::optional<uint32_t> seed_override;
  std::optional<std::pair<int, int>> res_override;
  std::optional<std::string> out_override;
  std::optional<std::string> map_dir;
  bool preview_only = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "mapview: %s needs an argument\n", flag);
      return std::nullopt;
    };
    if (a == "--preview-image-only") {
      preview_only = true;
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
    } else if (a == "--map") {
      if (auto v = next("--map")) map_dir = *v; else return 2;
    } else if (is_app_flag_with_value(a)) {
      if (!next(a.c_str())) return 2;  // consume the value; SdlViewerApp reads it
    } else {
      std::fprintf(stderr, "mapview: unknown arg '%s'\n", a.c_str());
      return 2;
    }
  }

  // An authored map's extent comes from its map_meta.json, so --resolution would be
  // silently ignored. Say so instead of pretending to honour it.
  if (map_dir && res_override) {
    std::fprintf(stderr,
                 "mapview: --resolution cannot be used with --map; an authored "
                 "map's size comes from its map_meta.json\n");
    return 2;
  }

  MapgenConfig cfg = badlands::mapgen::load_config(config_path);
  if (seed_override) cfg.seed = *seed_override;
  if (res_override) {
    cfg.width = res_override->first;
    cfg.height = res_override->second;
  }
  if (out_override) cfg.out_dir = *out_override;
  if (map_dir) cfg.map_dir = *map_dir;

  // Only meaningful for a generated map: an authored one validates its own dims
  // against kSamplesPerBlock at load, and rejects a bad size rather than warning.
  if (cfg.map_dir.empty() &&
      (cfg.width % badlands::mapgen::kSamplesPerBlock != 0 ||
       cfg.height % badlands::mapgen::kSamplesPerBlock != 0)) {
    std::fprintf(stderr,
                 "mapview: resolution %dx%d is not a multiple of %d; remainder "
                 "samples are dropped from the block grid\n",
                 cfg.width, cfg.height, badlands::mapgen::kSamplesPerBlock);
  }

  if (preview_only) return RunPreviewOnly(cfg);

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv, [cfg](const badlands::RenderContext&) {
    return std::make_unique<badlands::MapViewView>(cfg);
  });
}
