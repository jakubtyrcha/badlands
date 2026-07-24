// badlands_mapview: the map tool. Two modes, one generator.
//
//   --preview-image-only   run the generator and dump the debug rasters
//                          (bedrock/biome/heightmap PNGs) into --out, then
//                          exit. Pure CPU: no window, no GPU.
//   (default)              generate the map and render it as the in-game
//                          terrain (cluster-LOD, biome-colored) with a
//                          fixed-angle camera.
//
// Run from the repo root (shaders/ and assets/ resolve relative to cwd).
//
// Usage: badlands_mapview [--seed N] [--resolution WxH] [--size WxH] [--out DIR]
//                         [--preview-image-only]
//                         [--screenshot out.png] [--record dir/]
//
//   --resolution WxH  map texels (default 512x512)
//   --size WxH        map extent in world METERS (default 512x512). Texels must
//                     come out square: size.x/res.x == size.y/res.y.
//   --camera-height H starting camera height in metres (headless framing: a
//                     small H for a near shot, a large one for a far shot).
//   --lod-tint N      debug tint mode for cluster terrain: 0 shaded (default),
//                     1 per-triangle position hash, 2 LOD level.
//   --serial-build    build the cluster DAG single-threaded (default: parallel).
//                     The output DAG is bit-identical either way; this is the
//                     perf A/B baseline (build time shows in the stats log).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/app/sdl_viewer_app.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/outputs.hpp"
#include "executables/mapview/map_view_view.hpp"

namespace {

using badlands::mapgen::MapGenParams;

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
  const badlands::mapgen::MapArtifacts artifacts =
      badlands::mapgen::generate_map(params);
  std::printf("mapview: %dx%d texels, %.0fx%.0f m, seed=%u -> %s\n",
              params.resolution.x, params.resolution.y, params.size_m.x,
              params.size_m.y, params.seed, out_dir.c_str());
  badlands::mapgen::write_preview_images(out_dir, artifacts);
  std::printf("mapview: done (%s)\n", out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  MapGenParams params;
  std::string out_dir = "mapgen_out";
  bool preview_only = false;
  float camera_height = 0.0f;  // 0 = keep the default framing
  int lod_tint = 0;            // 0 shaded / 1 triangle hash / 2 LOD level
  bool serial_build = false;   // force single-threaded DAG build (perf A/B)

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
      params.resolution = {r->first, r->second};
    } else if (a == "--size") {
      auto v = next("--size");
      if (!v) return 2;
      auto r = parse_wxh<float>(*v, [](const std::string& t) { return std::stof(t); });
      if (!r) {
        std::fprintf(stderr, "mapview: bad --size '%s' (want WxH meters)\n",
                     v->c_str());
        return 2;
      }
      params.size_m = {r->first, r->second};
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

  // The frozen MapData lattice has ONE spacing scalar, so texels must be
  // square. Reject the contradiction instead of silently distorting the map.
  const float tx = params.size_m.x / static_cast<float>(params.resolution.x);
  const float ty = params.size_m.y / static_cast<float>(params.resolution.y);
  if (std::abs(tx - ty) > 1e-4f * std::max(tx, ty)) {
    std::fprintf(stderr,
                 "mapview: non-square texels (%.4f x %.4f m) — pick "
                 "--resolution/--size with matching aspect\n",
                 tx, ty);
    return 2;
  }

  if (preview_only) return RunPreviewOnly(params, out_dir);

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [params, camera_height, lod_tint,
                  serial_build](const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(
                       params, camera_height, lod_tint, serial_build);
                 });
}
