// badlands_mapview: generate a map and view its terrain (chunked, biome-blended)
// with a fixed-angle camera. Run from the repo root (scripts/mapgen/fields.noiser
// + assets/shaders resolve relative to cwd).
//
// Usage: badlands_mapview [--seed N] [--resolution N] [--screenshot out.png]

#include <cstdlib>
#include <cstring>
#include <memory>

#include "engine/app/sdl_viewer_app.hpp"
#include "mapview/map_view_view.hpp"

int main(int argc, char** argv) {
  uint32_t seed = 1;
  int resolution = 1000;  // samples per side (meters at 1 m density)
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
      resolution = std::atoi(argv[++i]);  // leading integer (WxH -> W)
    }
  }
  if (resolution < 10) resolution = 1000;

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [seed, resolution](const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(seed,
                                                                  resolution);
                 });
}
