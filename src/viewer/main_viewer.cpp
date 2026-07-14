#include <cstdlib>
#include <cstring>
#include <memory>

#include "engine/app/sdl_viewer_app.hpp"
#include "viewer/model_viewer_view.hpp"

int main(int argc, char** argv) {
  // Temporary headless-verification aid (Task S2.E): selects the initial
  // prefab shown, so a `--screenshot` run can capture a specific rock/
  // building without driving the ImGui combo interactively. Not part of the
  // stable CLI surface.
  int prefab_index = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--prefab") == 0 && i + 1 < argc) {
      prefab_index = std::atoi(argv[++i]);
    }
  }

  badlands::SdlViewerApp app({.window_title = "badlands_viewer"});
  return app.Run(argc, argv, [prefab_index](const badlands::RenderContext& /*ctx*/) {
    auto view = std::make_unique<badlands::ModelViewerView>();
    view->SetInitialPrefabIndex(prefab_index);
    return view;
  });
}
