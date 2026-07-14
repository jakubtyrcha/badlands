#pragma once

// Task S2.E: badlands_viewer's real AppView -- an orbit camera around a
// selected prefab (rocks + buildings) on a neutral gray floor, textured via
// MaterialLibrary + the building catalog, lit by the shared live-editable
// LightEnvironment. Replaces PlaceholderView for the viewer executable.
//
// Lives in src/viewer/ (an app, not the engine): it uses game types
// (GameBuildingKind, GamePloppableKind, building_visual, ploppable_local_ring,
// MaterialId, material_pack) to build its prefab catalog, which CLAUDE.md's
// layer boundary reserves for src/game / app code, not src/engine.

#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>

#include "badlands_game.h"  // GameBuildingKind, GAME_BUILDING_KIND_COUNT
#include "engine/app/app_view.hpp"
#include "engine/app/orbit_camera_controller.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/scene/scene_graph.hpp"
#include "game/geometry/ploppable_rings.h"  // GamePloppableKind

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

  // Selects the prefab shown once Initialize() builds the catalog + scene.
  // Must be called before Initialize() -- main_viewer.cpp's `--prefab <n>`
  // CLI arg uses it for headless screenshot verification. Out-of-range
  // indices are clamped in Initialize().
  void SetInitialPrefabIndex(int index) { prefab_index_ = index; }

 private:
  enum class PrefabCategory { Rock, Building };

  struct PrefabEntry {
    std::string label;
    PrefabCategory category;
    GamePloppableKind rock_kind{};
    GameBuildingKind building_kind{};
  };

  void BuildCatalog();
  // Re-derives env_'s sky cube / SH ambient / sun into scene_context_, then
  // mirrors the result into scene_ (see RebuildScene's comment for why the
  // mirroring is duplicated at both call sites).
  void ApplyEnvironment();
  // Clears scene_ and rebuilds it from scratch: re-mirrors scene_context_'s
  // lighting (SceneGraph defaults would otherwise clobber it), adds the gray
  // floor, adds the current catalog_[prefab_index_] prefab, and reframes the
  // orbit camera on the prefab's bounds. Called by Initialize() and whenever
  // DrawUI's prefab combo selection changes.
  void RebuildScene();
  // Adds `entry`'s entities to scene_ (one for a rock, one per building part)
  // and returns their combined LOCAL-space bounds (entities are placed at the
  // scene origin, untransformed) for orbit_.FrameBounds().
  Aabb AddPrefab(const PrefabEntry& entry);

  // GPU handles (from RenderContext, stored so DrawUI can re-run
  // ApplyEnvironment when the light-environment editor changes env_ live).
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

  std::vector<PrefabEntry> catalog_;
  int prefab_index_ = 0;

  bool left_mouse_down_ = false;
  float dt_ = 0.0f;
};

}  // namespace badlands
