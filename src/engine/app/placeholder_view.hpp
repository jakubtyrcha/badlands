#pragma once

// Task S2.A1: shared, game-agnostic AppView that reproduces Stage 1's single
// lit textured sphere through the app framework. Used by all three Stage-2
// executables (badlands_viewer/game/ai_sandbox) until each grows its own
// real view.

#include <memory>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/app/app_view.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class PlaceholderView : public AppView {
 public:
  void Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  LoadedTexture albedo_;
  wgpu::Sampler sampler_;
  std::unique_ptr<MaterialInstanceFactory> factory_;
  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
};

}  // namespace badlands
