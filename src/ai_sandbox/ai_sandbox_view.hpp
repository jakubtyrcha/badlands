#pragma once

// Task S2.G: badlands_ai_sandbox's real AppView -- a walled arena (reusing
// the game grid coordinate system via game/arena.h) with a gray floor, a
// blocked outer wall ring, and 2 static colored capsules, rendered through
// the shared deferred renderer + LightEnvironment, viewed with the shared
// fixed-angle GameCameraController (same as badlands_game's GameView).
// Replaces PlaceholderView for the ai_sandbox executable. No AI/nav movement
// this stage -- the capsules are placeholder units; AI/nav wiring is a
// follow-up.

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/game_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "game/arena.h"

namespace badlands {

class AiSandboxView : public AppView {
 public:
  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  // Re-derives env_'s sky cube / SH ambient / sun into scene_context_, then
  // mirrors the result into scene_ (its per-frame SyncToRegistry would
  // otherwise clobber it with SceneGraph's own defaults) -- same pattern as
  // GameView::ApplyEnvironment.
  void ApplyEnvironment();
  // Clears scene_ and rebuilds it from arena_: re-mirrors scene_context_'s
  // lighting, then adds the floor + wall ring + 2 capsules. Called once from
  // Initialize -- this stage's arena is static.
  void BuildScene();
  void AddWalls();
  void AddCapsules();
  // Centers the game camera on the arena origin and picks a height (at
  // GameCameraController's fixed pitch) so the whole arena -- including the
  // wall ring -- stays inside the frustum. The framing is aspect-independent
  // (it uses fixed empirical coefficients, not camera_.aspect), so it is run
  // once from Initialize; OnResize only refreshes camera_.aspect and must NOT
  // re-run this (it resets gamecam_.focus, discarding any WASD pan).
  void FrameCamera();

  // GPU handles (from RenderContext, stored so DrawUI can re-run
  // ApplyEnvironment when the light-environment editor changes env_ live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  // Floor + capsule materials are cached solid-color deferred materials from
  // MaterialLibrary::SolidColor (the library owns their 1x1 textures), so no
  // per-view texture/sampler handles are needed here anymore.

  Arena arena_;
  // World-space XZ positions of the 2 capsules, recorded by AddCapsules()
  // for the "Arena" UI window.
  glm::vec2 capsule_a_pos_{0.0f};
  glm::vec2 capsule_b_pos_{0.0f};

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  GameCameraController gamecam_;

  float dt_ = 0.0f;
};

}  // namespace badlands
