#include <memory>

#include "engine/app/placeholder_view.hpp"
#include "engine/app/sdl_viewer_app.hpp"

int main(int argc, char** argv) {
  badlands::SdlViewerApp app({.window_title = "badlands_viewer"});
  return app.Run(argc, argv, [](const badlands::RenderContext& ctx) {
    return std::make_unique<badlands::PlaceholderView>();
  });
}
