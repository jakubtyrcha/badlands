// badlands_mapview: the map tool. Two modes, one generator.
//
//   --preview-image-only   run the generator and dump a numbered PNG per
//                          pipeline stage plus the legacy preview rasters
//                          into --out, then exit. Pure CPU: no window, no GPU.
//   --load DIR             render a map LOADED from rasters on disk instead of
//                          generating one (see mapgen/map_io.hpp for the form).
//                          --resolution/--size come from the manifest.
//   (default)              generate the map and render it as the in-game
//                          terrain (cluster-LOD, biome-colored) with a
//                          fixed-angle camera.
//
// Run from the repo root (shaders/ and assets/ resolve relative to cwd).
//
// Usage: badlands_mapview [--seed N] [--resolution WxH] [--size WxH] [--out DIR]
//                         [--preview-image-only] [--load DIR]
//                         [--screenshot out.png] [--record dir/]
//
//   --resolution WxH  map texels, square only: W must equal H (default 512x512)
//   --size WxH        map extent in world METERS, square only: W must equal H
//                     (default 512x512)
//   --camera-height H starting camera height in metres (headless framing: a
//                     small H for a near shot, a large one for a far shot).
//   --lod-tint N      debug tint mode for cluster terrain: 0 shaded (default),
//                     1 per-triangle position hash, 2 LOD level.
//   --serial-build    build the cluster DAG single-threaded (default: parallel).
//                     The output DAG is bit-identical either way; this is the
//                     perf A/B baseline (build time shows in the stats log).
//   --test-map        skip the generator and load the synthetic 128 m forest
//                     map instead. It exists because classify_biomes emits no
//                     Biome::Forest, so a GENERATED map has no forest for the
//                     plopper to plant into and renders no trees at all.
//                     --seed still applies (it varies the terrain and the
//                     forest, not the forest's outline); --resolution/--size
//                     are ignored, the map is always 128x128 m.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/app/sdl_viewer_app.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/map_io.hpp"
#include "mapgen/outputs.hpp"
#include "executables/mapview/map_view_view.hpp"

namespace {

using badlands::mapgen::MapGenParams;

constexpr const char* kNonSquareMapError =
    "mapview: non-square maps are not supported\n";

// "WxH" -> the two values via `conv` (stoi for texels, stof for meters).
template <typename T, typename Conv>
std::optional<std::pair<T, T>> parse_wxh(const std::string& s, Conv conv) {
  auto x = s.find('x');
  if (x == std::string::npos) return std::nullopt;
  try {
    T w = conv(s.substr(0, x));
    T h = conv(s.substr(x + 1));
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
int RunPreviewOnly(const MapGenParams& params, const std::string& out_dir) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "mapview: cannot create out dir '%s': %s\n",
                 out_dir.c_str(), ec.message().c_str());
    return 1;
  }
  const float sim_texel_m =
      params.world_size_m / static_cast<float>(params.erosion.sim_resolution);
  const float out_texel_m =
      params.world_size_m / static_cast<float>(params.resolution);
  badlands::mapgen::PngDebugSink sink(out_dir, sim_texel_m, out_texel_m);
  const badlands::mapgen::MapArtifacts artifacts =
      badlands::mapgen::generate_map(params, &sink);
  std::printf("mapview: %dx%d texels, %.0fx%.0f m, seed=%u -> %s\n",
              params.resolution, params.resolution, params.world_size_m,
              params.world_size_m, params.seed, out_dir.c_str());
  badlands::mapgen::write_preview_images(
      out_dir, artifacts,
      params.world_size_m / static_cast<float>(params.resolution));
  std::printf("mapview: done (%s)\n", out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  MapGenParams params;
  std::string out_dir = "mapgen_out";
  std::string load_dir;
  bool preview_only = false;
  float camera_height = 0.0f;  // 0 = keep the default framing
  int lod_tint = 0;            // 0 shaded / 1 triangle hash / 2 LOD level
  bool serial_build = false;   // force single-threaded DAG build (perf A/B)
  bool test_map = false;       // synthetic forest map instead of the generator

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "mapview: %s needs an argument\n", flag);
      return std::nullopt;
    };
    // Parse one numeric flag value with `conv` (stoul/stof/stoi), reporting
    // `want` on a bad value. Returns false (caller returns 2) on missing/bad.
    auto parse_num = [&](const char* flag, const char* want, auto conv,
                         auto& out) -> bool {
      auto v = next(flag);
      if (!v) return false;
      try {
        out = conv(*v);
        return true;
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad %s '%s' (want %s)\n", flag, v->c_str(),
                     want);
        return false;
      }
    };
    if (a == "--preview-image-only") {
      preview_only = true;
    } else if (a == "--serial-build") {
      serial_build = true;
    } else if (a == "--test-map") {
      test_map = true;
    } else if (a == "--seed") {
      if (!parse_num(
              "--seed", "a number",
              [](const std::string& s) { return static_cast<uint32_t>(std::stoul(s)); },
              params.seed))
        return 2;
    } else if (a == "--resolution") {
      auto v = next("--resolution");
      if (!v) return 2;
      auto r = parse_wxh<int>(*v, [](const std::string& t) { return std::stoi(t); });
      if (!r) {
        std::fprintf(stderr, "mapview: bad --resolution '%s' (want WxH texels)\n",
                     v->c_str());
        return 2;
      }
      if (r->first != r->second) {
        std::fprintf(stderr, "%s", kNonSquareMapError);
        return 2;
      }
      params.resolution = r->first;
    } else if (a == "--size") {
      auto v = next("--size");
      if (!v) return 2;
      auto r = parse_wxh<float>(*v, [](const std::string& t) { return std::stof(t); });
      if (!r) {
        std::fprintf(stderr, "mapview: bad --size '%s' (want WxH meters)\n",
                     v->c_str());
        return 2;
      }
      if (r->first != r->second) {
        std::fprintf(stderr, "%s", kNonSquareMapError);
        return 2;
      }
      params.world_size_m = r->first;
    } else if (a == "--load") {
      if (auto v = next("--load")) load_dir = *v; else return 2;
    } else if (a == "--out") {
      if (auto v = next("--out")) out_dir = *v; else return 2;
    } else if (a == "--camera-height") {
      if (!parse_num(
              "--camera-height", "metres",
              [](const std::string& s) { return std::stof(s); }, camera_height))
        return 2;
    } else if (a == "--lod-tint") {
      if (!parse_num(
              "--lod-tint", "0, 1, or 2",
              [](const std::string& s) { return std::stoi(s); }, lod_tint))
        return 2;
      if (lod_tint < 0 || lod_tint > 2) {
        std::fprintf(stderr, "mapview: --lod-tint %d out of range (want 0..2)\n",
                     lod_tint);
        return 2;
      }
    } else if (is_app_flag_with_value(a)) {
      if (!next(a.c_str())) return 2;  // consume the value; SdlViewerApp reads it
    } else {
      std::fprintf(stderr, "mapview: unknown arg '%s'\n", a.c_str());
      return 2;
    }
  }

  if (preview_only) {
    // The preview dump renders the GENERATOR's debug rasters, and neither the
    // loaded map nor the test map produces any. Combining the flags asks for
    // something that does not exist, so say so rather than dumping a blank set.
    if (!load_dir.empty() || test_map) {
      std::fprintf(stderr,
                   "mapview: --preview-image-only needs the generator; %s has no "
                   "preview rasters\n",
                   load_dir.empty() ? "--test-map" : "--load");
      return 2;
    }
    return RunPreviewOnly(params, out_dir);
  }

  if (!load_dir.empty() && test_map) {
    std::fprintf(stderr,
                 "mapview: --load and --test-map are exclusive -- each names a "
                 "different map\n");
    return 2;
  }

  // Read the manifest BEFORE constructing the view and write the geometry into
  // `params`, so resolution/world_size_m have one source of truth and every
  // existing params_ use inside MapViewView (splat texel size, camera framing)
  // stays correct on the load path.
  if (!load_dir.empty()) {
    std::string err;
    const auto man = badlands::mapgen::load_manifest(load_dir, &err);
    if (!man) {
      std::fprintf(stderr, "mapview: %s\n", err.c_str());
      return 1;
    }
    params.resolution = man->resolution;
    params.world_size_m = man->world_size_m;
    std::printf("mapview: loading %s (%dx%d texels, %.0f m, %.2f m/texel)\n",
                load_dir.c_str(), man->resolution, man->resolution,
                man->world_size_m,
                man->world_size_m / static_cast<float>(man->resolution));
    if (!man->source.empty())
      std::printf("mapview:   source: %s\n", man->source.c_str());
  }

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [params, camera_height, lod_tint, serial_build,
                  load_dir, test_map](const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(
                       params, camera_height, lod_tint, serial_build, load_dir,
                       test_map);
                 });
}
