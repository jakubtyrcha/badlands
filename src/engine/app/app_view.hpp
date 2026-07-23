#pragma once

// Task S2.A1: game-agnostic app framework. See sdl_viewer_app.hpp for the
// framework overview.

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "engine/app/render_context.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"

namespace badlands {

class UiRenderer;

// A single application view. Owns its scene, camera, and UI. Views share the
// renderer + engine. Game-agnostic base (no game types).
class AppView {
 public:
  virtual ~AppView() = default;

  // Called once after the GPU/renderer exist. Build scene + materials here.
  // Returns false (after logging) if setup failed (e.g. a material factory
  // could not be built); the app aborts with a nonzero exit code rather than
  // rendering an empty scene.
  virtual bool Initialize(const RenderContext& ctx) = 0;

  // One SDL event. (ImGui gating is added in A2.)
  virtual void HandleEvent(const SDL_Event& event, int width, int height) = 0;

  // Per-frame update, driven by the render loop with a real-time `dt` (wall
  // time live, or a fixed presentation step when recording). The view owns its
  // own time model: it advances its SimClock from `dt` (applying sim speed),
  // steps its fixed-rate game logic and time-driven visuals (e.g. day/night)
  // off that clock, updates the camera, then syncs its scene into
  // GetRegistry()/GetSceneContext() so the app can render it. Sim vs render
  // decoupling lives inside the view (fixed sim ticks vs continuous visuals),
  // not in the loop — see sim_clock.hpp.
  virtual void Update(float dt, const bool* keyboard_state) = 0;

  // Headless determinism hook: jump any presentation clock (e.g. day/night) to
  // an absolute normalized time-of-day t in [0,1) and refresh derived state, so
  // --screenshot renders a chosen time and captures are reproducible. Default
  // no-op (views without a time-of-day).
  virtual void SeekToTimeOfDay(float t01) { (void)t01; }

  // Per-frame ImGui windows (no-op until A2). This is the DEBUG UI surface.
  virtual void DrawUI() {}

  // The view's GAME UI renderer, or nullptr if it has none. The two UI
  // surfaces are deliberately separate (see CLAUDE.md): DrawUI() above is Dear
  // ImGui debug UI; this is the in-world game UI, drawn by the app into a
  // screen-space overlay pass after the tonemap resolve and BEFORE ImGui, so
  // debug UI always sits on top.
  //
  // The VIEW owns the renderer (like its material factories, built in
  // Initialize(RenderContext)) because the glyph atlas + pipeline must be owned
  // together, and views with no game UI should not pay for an atlas. The APP
  // runs the pass, so no render-pass encoder leaks into view code.
  virtual UiRenderer* GetUiRenderer() { return nullptr; }

  virtual void OnResize(int width, int height) = 0;
  virtual void ResetCamera() {}

  virtual Camera& GetCamera() = 0;
  virtual entt::registry& GetRegistry() = 0;
  virtual SceneContext& GetSceneContext() = 0;
};

}  // namespace badlands
