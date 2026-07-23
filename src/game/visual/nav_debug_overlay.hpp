#pragma once

// Shared pathfinding debug overlay for the ImGui debug UI, used by both GameView
// and AiSandboxView. Owns the toggle state + a debug-line buffer; it draws the
// cost-coloured navmesh and a click-two-points routed path through the engine
// debug-line pass (SceneContext::debug_lines).
//
// The two hosts differ only in two spots, which they supply:
//   * the GROUND HEIGHT under a world XZ (flat 0 for the sandbox arena, terrain
//     height for the game) -- passed to Rebuild;
//   * how a ground PICK is captured (a flat-plane ray vs a terrain raycast) --
//     the host does the raycast and calls Pick() with the resulting world point.
// Everything else -- the drawing, the panel widgets, the A/B state -- is shared.

#include "badlands_sim.hpp"  // badlands::Sim + NavDebugCell + NavPathResult
#include "engine/rendering/debug_line_buffer.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace badlands {

struct SceneContext;  // engine/rendering/context/scene_context.hpp

class NavDebugOverlay {
 public:
  // Terrain height at a world XZ (the overlay adds its own small lift on top).
  using GroundHeightFn = std::function<float(float x, float z)>;

  // Rebuild the debug lines from the sim's navmesh + the picked path and point
  // ctx.debug_lines at them (nullptr when nothing is drawn). Call once per frame.
  void Rebuild(Sim& sim, SceneContext& ctx, const GroundHeightFn& ground_y);

  // A ground pick (world XZ) while pick mode is on: sets endpoint A, then B, then
  // restarts. The host gates this on pick_mode() and does its own raycast.
  void Pick(glm::vec2 world_xz);
  bool pick_mode() const { return pick_mode_; }

  // The ImGui widgets (toggles + path readout + clear). The caller places them in
  // its own window / collapsing header.
  void DrawControls();

 private:
  DebugLineBuffer lines_;
  std::vector<NavDebugCell> cells_;  // reused snapshot buffer
  bool show_mesh_ = false;
  bool pick_mode_ = false;
  std::optional<glm::vec2> a_;  // path endpoints (world XZ)
  std::optional<glm::vec2> b_;
  NavPathResult path_;  // last query (cost / reachable)
};

}  // namespace badlands
