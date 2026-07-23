#include <cstdlib>
#include <memory>

#include <spdlog/spdlog.h>

#include "engine/app/sdl_viewer_app.hpp"
#include "executables/game/game_view.hpp"
#include "game/visual/render_mode.hpp"

namespace {

// USE_BLOCKOUT_MODE (any non-empty value) renders the greybox blockout proxies
// (debug solid-color terrain + flat water + gray buildings) instead of the
// detailed PBR materials. Read once at startup.
badlands::RenderMode RenderModeFromEnv() {
  const char* v = std::getenv("USE_BLOCKOUT_MODE");
  const bool blockout = v != nullptr && v[0] != '\0';
  spdlog::info("badlands_game: render mode = {}",
               blockout ? "BLOCKOUT" : "detailed");
  return blockout ? badlands::RenderMode::Blockout
                  : badlands::RenderMode::Detailed;
}

}  // namespace

int main(int argc, char** argv) {
  const badlands::RenderMode mode = RenderModeFromEnv();
  badlands::SdlViewerApp app({.window_title = "badlands_game"});
  return app.Run(argc, argv, [mode](const badlands::RenderContext& /*ctx*/) {
    return std::make_unique<badlands::GameView>(mode);
  });
}
