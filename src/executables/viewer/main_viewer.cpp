#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "engine/app/sdl_viewer_app.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "executables/viewer/model_viewer_view.hpp"

int main(int argc, char** argv) {
  // Selects the initial generator shown, so a `--screenshot` run can capture a
  // specific mesh without driving the ImGui list. Not part of the stable CLI.
  int generator_index = 0;
  // Same category (Task T3): selects the initial ShadowDebugMode so a
  // `--screenshot` run can capture the shadow-map/contact-shadow debug masks
  // (0=Off, 1=Combined, 2=ShadowMapOnly, 3=ContactOnly) headlessly.
  auto shadow_debug_mode = badlands::ShadowDebugMode::Off;
  // Manual LOD switch (Task 2): selects the initial tree LOD level (0..2) so
  // a `--screenshot` run can capture a specific LOD headlessly.
  int lod = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--generator") == 0 && i + 1 < argc) {
      generator_index = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--shadow-debug") == 0 && i + 1 < argc) {
      shadow_debug_mode =
          static_cast<badlands::ShadowDebugMode>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--lod") == 0 && i + 1 < argc) {
      lod = std::clamp(std::atoi(argv[++i]), 0, 2);
    }
  }

  badlands::SdlViewerApp app({.window_title = "badlands_viewer"});
  return app.Run(argc, argv, [generator_index, shadow_debug_mode, lod](
                                 const badlands::RenderContext& /*ctx*/) {
    auto view = std::make_unique<badlands::ModelViewerView>();
    view->SetInitialGeneratorIndex(generator_index);
    view->SetInitialShadowDebugMode(shadow_debug_mode);
    view->SetInitialLod(lod);
    return view;
  });
}
