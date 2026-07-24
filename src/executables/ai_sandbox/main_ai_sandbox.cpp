#include <memory>

#include "executables/ai_sandbox/ai_sandbox_view.hpp"
#include "engine/app/sdl_viewer_app.hpp"

int main(int argc, char** argv) {
  badlands::SdlViewerApp app({.window_title = "badlands_ai_sandbox"});
  return app.Run(argc, argv, [](const badlands::RenderContext& /*ctx*/) {
    return std::make_unique<badlands::AiSandboxView>();
  });
}
