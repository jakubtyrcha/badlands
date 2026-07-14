#include <memory>

#include "engine/app/sdl_viewer_app.hpp"
#include "game/views/game_view.hpp"

int main(int argc, char** argv) {
  badlands::SdlViewerApp app({.window_title = "badlands_game"});
  return app.Run(argc, argv, [](const badlands::RenderContext& /*ctx*/) {
    return std::make_unique<badlands::GameView>();
  });
}
