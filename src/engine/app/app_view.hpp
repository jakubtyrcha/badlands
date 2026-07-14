#pragma once

// Task S2.A1: game-agnostic app framework. See sdl_viewer_app.hpp for the
// framework overview.

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "engine/app/render_context.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"

namespace badlands {

// A single application view. Owns its scene, camera, and UI. Views share the
// renderer + engine. Game-agnostic base (no game types).
class AppView {
 public:
  virtual ~AppView() = default;

  // Called once after the GPU/renderer exist. Build scene + materials here.
  virtual void Initialize(const RenderContext& ctx) = 0;

  // One SDL event. (ImGui gating is added in A2.)
  virtual void HandleEvent(const SDL_Event& event, int width, int height) = 0;

  // Per-frame update: sim/camera + sync this view's scene into GetRegistry()/
  // GetSceneContext() so the app can render it.
  virtual void Update(float dt, const bool* keyboard_state) = 0;

  // Per-frame ImGui windows (no-op until A2).
  virtual void DrawUI() {}

  virtual void OnResize(int width, int height) = 0;
  virtual void ResetCamera() {}

  virtual Camera& GetCamera() = 0;
  virtual entt::registry& GetRegistry() = 0;
  virtual SceneContext& GetSceneContext() = 0;
};

}  // namespace badlands
