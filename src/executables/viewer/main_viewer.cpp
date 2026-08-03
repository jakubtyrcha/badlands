#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "engine/app/sdl_viewer_app.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "executables/viewer/character_viewer_view.hpp"
#include "executables/viewer/model_viewer_view.hpp"

int main(int argc, char** argv) {
  // Which of the app's two AppViews to run: the foliage/LOD mesh viewer
  // (default) or the character/skeleton viewer (`--character`). The choice is
  // made once here because SdlViewerApp takes a view factory, not a switchable
  // view -- there is no in-session toggle between them.
  bool character_mode = false;
  // Character viewer only: initial clip by LOGICAL name (an
  // assets/characters/*/clips.json key, e.g. "walk"), and a pinned playback
  // ratio so a `--screenshot` run captures a deterministic frame. Same
  // category as --generator/--lod below: screenshot affordances, not a
  // stable CLI.
  std::string clip_name;
  // Whether the flag was SEEN is tracked separately from its value: the value
  // is clamped into [0,1], so no in-band sentinel could survive to mean
  // "unset", and `--anim-time -1` would silently pin frame 0 instead of playing.
  bool anim_time_set = false;
  float anim_time = 0.0f;
  // Selects the initial generator shown, so a `--screenshot` run can capture a
  // specific mesh without driving the ImGui list. Not part of the stable CLI.
  int generator_index = 0;
  // Same category (Task T3): selects the initial ShadowDebugMode so a
  // `--screenshot` run can capture the shadow-map/contact-shadow debug masks
  // (0=Off, 1=Combined, 2=ShadowMapOnly, 3=ContactOnly) headlessly.
  auto shadow_debug_mode = badlands::ShadowDebugMode::Off;
  // Manual LOD switch (Task 2): selects the initial tree LOD level so a
  // `--screenshot` run can capture a specific LOD headlessly. 0 =
  // "Original"; 1..kVoxelLodCount = "Voxel L0..L3" (volumetric-foliage
  // Phase 3, one per kFoliageVoxelWorldSizes cell size); kMultiLodLevel =
  // "Multi", an instanced grid with dynamic GPU LOD (Task 4). The bound comes
  // from the view itself, so adding a voxel level needs no edit here.
  int lod = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--generator") == 0 && i + 1 < argc) {
      generator_index = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--shadow-debug") == 0 && i + 1 < argc) {
      shadow_debug_mode =
          static_cast<badlands::ShadowDebugMode>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--lod") == 0 && i + 1 < argc) {
      lod = std::clamp(std::atoi(argv[++i]), 0,
                       badlands::ModelViewerView::kMultiLodLevel);
    } else if (std::strcmp(argv[i], "--character") == 0) {
      character_mode = true;
    } else if (std::strcmp(argv[i], "--clip") == 0 && i + 1 < argc) {
      clip_name = argv[++i];
      character_mode = true;  // naming a clip implies the character viewer
    } else if (std::strcmp(argv[i], "--anim-time") == 0 && i + 1 < argc) {
      anim_time = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
      anim_time_set = true;
      character_mode = true;
    }
  }

  badlands::SdlViewerApp app({.window_title = "badlands_viewer"});
  return app.Run(
      argc, argv,
      [character_mode, clip_name, anim_time, anim_time_set, generator_index,
       shadow_debug_mode, lod](const badlands::RenderContext& /*ctx*/)
          -> std::unique_ptr<badlands::AppView> {
        if (character_mode) {
          auto view = std::make_unique<badlands::CharacterViewerView>();
          if (!clip_name.empty()) view->SetInitialClipName(clip_name);
          if (anim_time_set) view->SetFixedAnimTime(anim_time);
          return view;
        }
        auto view = std::make_unique<badlands::ModelViewerView>();
        view->SetInitialGeneratorIndex(generator_index);
        view->SetInitialShadowDebugMode(shadow_debug_mode);
        view->SetInitialLod(lod);
        return view;
      });
}
