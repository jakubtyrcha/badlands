#pragma once

// badlands_viewer's character AppView: one skeleton on a neutral floor, playing
// a clip from an AnimationSet, drawn as debug lines. Selected with --character;
// ModelViewerView (the foliage/LOD viewer) is the other view in this app.
//
// There is no character geometry yet, so the skeleton IS the presentation here.
// This view drives the engine animation runtime with no Sim present, which is
// exactly the separation src/engine/animation/ exists to keep.

#include <memory>
#include <optional>
#include <string>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "engine/animation/animation_set.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"
#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands {

class CharacterViewerView : public AppView {
 public:
  bool Initialize(const RenderContext& ctx) override;
  void HandleEvent(const SDL_Event& event, int width, int height) override;
  void Update(float dt, const bool* keyboard_state) override;
  void DrawUI() override;
  void OnResize(int width, int height) override;

  Camera& GetCamera() override { return camera_; }
  entt::registry& GetRegistry() override { return registry_; }
  SceneContext& GetSceneContext() override { return scene_context_; }

  // Selects the clip shown by LOGICAL name (the AnimationSet manifest's key,
  // e.g. "walk"). Call before Initialize(); an unknown name is reported and
  // falls back to the first clip. Backs `--clip`.
  void SetInitialClipName(std::string name) { initial_clip_name_ = std::move(name); }

  // Pins playback at a fixed ratio in [0,1] instead of advancing with time, so
  // a headless `--screenshot` captures a deterministic frame. Backs
  // `--anim-time`; unset means play.
  void SetFixedAnimTime(float ratio) { fixed_ratio_ = ratio; }

  // Overrides the character asset manifest (default
  // assets/characters/quaternius/clips.json). Call before Initialize().
  // Backs `--rig`.
  void SetManifestPath(std::string path) { manifest_path_ = std::move(path); }

 private:
  void ApplyEnvironment();
  void BuildScene();
  // Samples the current clip and refills skeleton_lines_ from it.
  void UpdateSkeleton();

  wgpu::Device device_;
  wgpu::Queue queue_;
  SceneRenderer* scene_renderer_ = nullptr;
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  Camera camera_;
  OrbitCameraController orbit_;
  entt::registry registry_;
  SceneContext scene_context_;
  SceneGraph scene_;
  MaterialLibrary matlib_;
  LightEnvironment env_;
  CubemapBuilder sky_cube_;

  std::string manifest_path_ = "assets/characters/quaternius/clips.json";
  std::optional<AnimationSet> animation_;
  std::optional<Pose> pose_;
  ClipSampler sampler_;
  DebugLineBuffer skeleton_lines_;

  std::string initial_clip_name_;
  int clip_index_ = 0;
  // Substring filter over the clip list. An imported 0 A.D. family declares 651
  // clips, which is a haystack rather than a list without it.
  char clip_filter_[64] = {};
  // Presentation time within the clip, in seconds. anim_* by the project's
  // clock rules: this is a view, so it advances on presentation time and never
  // on anything the sim would recognise.
  float anim_seconds_ = 0.0f;
  float playback_scale_ = 1.0f;
  bool playing_ = true;
  std::optional<float> fixed_ratio_;
  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
