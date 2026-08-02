// badlands_mapview: the map tool. Exactly one of three ways to get a map:
//
//   --load DIR     render a patch LOADED from rasters on disk (see
//                  mapgen/patch_io.hpp for the on-disk form) -- a window
//                  simulated by tools/protogen/. Geometry (resolution,
//                  world_size_m, origin_m) comes from the manifest, not the
//                  CLI.
//   --synthetic    render a patch invented analytically
//                  (mapgen/synthetic_patch_source.hpp) -- no files, no
//                  upstream stage. Geometry comes from --patch-size /
//                  --patch-res / --patch-origin.
//   --test-map     skip the patch source entirely and render the synthetic
//                  128 m forest map (game/map/forest_test_map_generator.hpp).
//                  It exists to give the forest plopper a Biome::Forest to
//                  plant into, which a fetched patch is not guaranteed to
//                  have.
//
// Run from the repo root (shaders/ and assets/ resolve relative to cwd).
//
// Usage: badlands_mapview (--load DIR | --synthetic | --test-map)
//                         [--patch-size M] [--patch-res N]
//                         [--patch-origin X,Y] [--seed N]
//                         [--camera-height H] [--lod-tint N] [--serial-build]
//                         [--screenshot out.png] [--record dir/]
//
//   --patch-size M     --synthetic patch extent in metres (default 128).
//   --patch-res N      --synthetic patch resolution in texels (default 128).
//   --patch-origin X,Y --synthetic patch origin in metres (default 0,0).
//   --seed N          seeds foliage placement (and, with --test-map, the
//                      synthetic forest layout) -- never terrain.
//   --camera-height H starting camera height in metres (headless framing: a
//                     small H for a near shot, a large one for a far shot).
//   --lod-tint N      debug tint mode for cluster terrain: 0 shaded (default),
//                     1 per-triangle position hash, 2 LOD level.
//   --serial-build    build the cluster DAG single-threaded (default: parallel).
//                     The output DAG is bit-identical either way; this is the
//                     perf A/B baseline (build time shows in the stats log).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "engine/app/sdl_viewer_app.hpp"
#include "executables/mapview/map_view_view.hpp"
#include "mapgen/file_patch_source.hpp"
#include "mapgen/synthetic_patch_source.hpp"

namespace {

constexpr const char* kUsage =
    "usage: badlands_mapview (--load DIR | --synthetic | --test-map) "
    "[--patch-size M] [--patch-res N] [--patch-origin X,Y] [--seed N] "
    "[--camera-height H] [--lod-tint N] [--serial-build] "
    "[--screenshot out.png] [--record dir/]\n";

// Flags owned by the app layer (SdlViewerApp::Run parses these out of the raw
// argv itself). We must skip them + their value rather than reject them as
// unknown, or --screenshot/--record would stop working here.
bool is_app_flag_with_value(const std::string& a) {
  return a == "--screenshot" || a == "--record";
}

// THE single named selection boundary: exactly one PatchSource (or none, for
// --test-map) and one PatchRequest, decided here and nowhere else -- main()
// hands the result straight to MapViewView without ever inspecting which flag
// won. Returns false (having already printed the reason to stderr) if --load
// named a directory that failed to load; true otherwise, including --test-map
// (where `source` stays null and `request` stays default) and --synthetic.
bool SelectPatchSource(
    bool load, const std::string& load_dir, bool synthetic, float patch_size_m,
    int patch_res, glm::dvec2 patch_origin_m,
    std::shared_ptr<const badlands::mapgen::PatchSource>& source,
    badlands::mapgen::PatchRequest& request) {
  if (load) {
    std::string err;
    std::unique_ptr<badlands::mapgen::FilePatchSource> file_source =
        badlands::mapgen::LoadFilePatchSource(load_dir, &err);
    if (!file_source) {
      std::fprintf(stderr, "mapview: %s\n", err.c_str());
      return false;
    }
    // A directory is a finished artifact, not a queryable world -- Fetch
    // ignores the request and returns what the file holds, so the request we
    // hand back out is the geometry the file actually has.
    request = file_source->native_request();
    std::printf("mapview: loading %s (%dx%d texels, %.0f m, %.2f m/texel)\n",
                load_dir.c_str(), request.resolution, request.resolution,
                request.world_size_m,
                badlands::mapgen::patch_texel_m(request));
    if (!file_source->source().empty())
      std::printf("mapview:   source: %s\n", file_source->source().c_str());
    source = std::move(file_source);
  } else if (synthetic) {
    source = std::make_shared<badlands::mapgen::SyntheticPatchSource>();
    request.world_size_m = patch_size_m;
    request.resolution = patch_res;
    request.origin_m = patch_origin_m;
  }
  // else --test-map: source stays null, request stays default -- MapViewView
  // never fetches from it.
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool load = false, synthetic = false, test_map = false;
  std::string load_dir;
  float patch_size_m = 128.0f;
  int patch_res = 128;
  double patch_origin_x = 0.0, patch_origin_y = 0.0;
  uint32_t seed = 1;            // foliage placement, never terrain
  float camera_height = 0.0f;   // 0 = keep the default framing
  int lod_tint = 0;             // 0 shaded / 1 triangle hash / 2 LOD level
  bool serial_build = false;    // force single-threaded DAG build (perf A/B)

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
    if (a == "--serial-build") {
      serial_build = true;
    } else if (a == "--test-map") {
      test_map = true;
    } else if (a == "--synthetic") {
      synthetic = true;
    } else if (a == "--seed") {
      if (!parse_num(
              "--seed", "a number",
              [](const std::string& s) { return static_cast<uint32_t>(std::stoul(s)); },
              seed))
        return 2;
    } else if (a == "--load") {
      if (auto v = next("--load")) {
        load = true;
        load_dir = *v;
      } else {
        return 2;
      }
    } else if (a == "--patch-size") {
      if (!parse_num(
              "--patch-size", "metres",
              [](const std::string& s) { return std::stof(s); }, patch_size_m))
        return 2;
    } else if (a == "--patch-res") {
      if (!parse_num(
              "--patch-res", "texels",
              [](const std::string& s) { return std::stoi(s); }, patch_res))
        return 2;
    } else if (a == "--patch-origin") {
      auto v = next("--patch-origin");
      if (!v) return 2;
      const size_t comma = v->find(',');
      bool ok = comma != std::string::npos;
      if (ok) {
        try {
          patch_origin_x = std::stod(v->substr(0, comma));
          patch_origin_y = std::stod(v->substr(comma + 1));
        } catch (const std::exception&) {
          ok = false;
        }
      }
      if (!ok) {
        std::fprintf(stderr, "mapview: bad --patch-origin '%s' (want X,Y)\n",
                     v->c_str());
        return 2;
      }
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

  const int mode_count = (load ? 1 : 0) + (synthetic ? 1 : 0) + (test_map ? 1 : 0);
  if (mode_count != 1) {
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }

  std::shared_ptr<const badlands::mapgen::PatchSource> source;
  badlands::mapgen::PatchRequest request;
  if (!SelectPatchSource(load, load_dir, synthetic, patch_size_m, patch_res,
                         glm::dvec2(patch_origin_x, patch_origin_y), source,
                         request))
    return 1;

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [request, source, seed, camera_height, lod_tint, serial_build,
                  test_map](const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(
                       request, source, seed, camera_height, lod_tint,
                       serial_build, test_map);
                 });
}
