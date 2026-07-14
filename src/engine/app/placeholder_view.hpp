#pragma once

// Task S2.A1: shared, game-agnostic AppView that reproduces Stage 1's single
// lit textured sphere through the app framework. Used by all three Stage-2
// executables (badlands_viewer/game/ai_sandbox) until each grows its own
// real view.

#include <memory>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class PlaceholderView : public AppView {
 public:
  void Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

 private:
  // GPU handles (from RenderContext, stored so DrawUI can re-run
  // ApplyLightEnvironment when the EditorUI light-environment editor changes
  // env_ live).
  wgpu::Device device_;
  wgpu::Queue queue_;
  // Shared renderer (owned by the app, outlives this view) — DrawUI drives its
  // G-buffer debug selector via EditorUI.
  SceneRenderer* scene_renderer_ = nullptr;

  LoadedTexture albedo_;
  wgpu::Sampler sampler_;
  // Shared light environment (sun + analytic sky). ApplyLightEnvironment builds
  // sky_cube_ + the SH ambient + the sun from it in Initialize (replaces B2's
  // hardcoded inline sky/SH), and again live from DrawUI whenever the EditorUI
  // light-environment editor reports a change.
  LightEnvironment env_;
  CubemapBuilder sky_cube_;
  // Temporary verification aid: a ~0.4 solid-gray 1x1 roughness override so the
  // sphere is smooth enough to show a visible skybox reflection. Real
  // per-material roughness arrives with the material-pack loader later.
  wgpu::TextureView roughness_view_;
  std::unique_ptr<MaterialInstanceFactory> factory_;
  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  OrbitCameraController orbit_;
  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
