#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "engine/app/sdl_viewer_app.hpp"
#include "executables/ai_sandbox/ai_sandbox_view.hpp"
#include "executables/ai_sandbox/duel_mode.hpp"

namespace {

// Which mode to drive. Parsed here rather than in the engine: SdlViewerApp
// ignores flags it does not recognise, and the view factory can close over
// argv, so a mode selector costs no change to the app framework.
std::unique_ptr<badlands::SandboxMode> MakeMode(int argc, char** argv) {
  std::string name = "duel";
  uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    }
  }
  if (name != "duel") {
    spdlog::error("badlands_ai_sandbox: unknown --mode '{}' -- running duel", name);
  }
  badlands::DuelConfig cfg;
  cfg.seed = seed;
  return std::make_unique<badlands::DuelMode>(cfg);
}

}  // namespace

int main(int argc, char** argv) {
  badlands::SdlViewerApp app({.window_title = "badlands_ai_sandbox"});
  return app.Run(argc, argv, [argc, argv](const badlands::RenderContext& /*ctx*/) {
    return std::make_unique<badlands::AiSandboxView>(MakeMode(argc, argv));
  });
}
