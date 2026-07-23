#pragma once

// badlands_viewer's AppView: a procedural-mesh scaffold. An orbit camera around
// a single mesh produced by the selected generator, on a neutral gray floor,
// textured with a UV-checker debug material, lit by the simple LightEnvironment
// sun. generators_ is the extension point where future foliage/rock generators
// slot in. Lives in src/executables/viewer/ (an app, not the engine).

#include <functional>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // TexturedMeshResult
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_renderer.hpp"  // ShadowDebugMode
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class ModelViewerView : public AppView {
 public:
  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

  // Selects the generator shown once Initialize() builds the registry + scene.
  // Must be called before Initialize() -- main_viewer.cpp's `--generator <n>`
  // CLI arg uses it for headless screenshot verification. Out-of-range indices
  // are clamped in Initialize().
  void SetInitialGeneratorIndex(int index) { generator_index_ = index; }

  // Selects the initial ShadowDebugMode (headless `--shadow-debug <n>`:
  // 0=Off, 1=Combined, 2=ShadowMapOnly, 3=ContactOnly). Call before Initialize().
  void SetInitialShadowDebugMode(ShadowDebugMode mode) {
    initial_shadow_debug_mode_ = mode;
  }

 private:
  // The output of a generator: a mesh plus the transform that places it. The
  // generator assumes the floor is at y=0 and returns the resting offset as a
  // transform -- it does NOT bake the offset into the mesh vertices, so the
  // mesh stays in its own natural local space.
  struct GeneratedMesh {
    TexturedMeshResult mesh;
    glm::mat4 transform{1.0f};
  };
  // A named procedural-mesh generator: produces one GeneratedMesh for the scene.
  struct MeshGenerator {
    std::string name;
    std::function<GeneratedMesh()> generate;
  };

  void BuildGenerators();
  // Re-derives env_'s sky/SH/sun into scene_context_ and mirrors it into scene_.
  void ApplyEnvironment();
  // Fresh graph: re-mirror lighting, add the gray floor at y=0, run the selected
  // generator, add its mesh under the generator's transform with checker_mat_,
  // and reframe the orbit on the mesh's world-space bounds.
  void RebuildScene();

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;

  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  SceneGraph scene_;
  entt::registry registry_;
  SceneContext scene_context_;
  Camera camera_;
  OrbitCameraController orbit_;

  std::vector<MeshGenerator> generators_;
  int generator_index_ = 0;
  DeferredMaterial checker_mat_;  // UV-checker debug material for the object

  ShadowDebugMode initial_shadow_debug_mode_ = ShadowDebugMode::Off;

  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
