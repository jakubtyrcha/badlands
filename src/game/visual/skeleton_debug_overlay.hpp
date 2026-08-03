#pragma once

// Shared character-skeleton debug overlay for the ImGui debug UI, used by both
// GameView and AiSandboxView. Modelled on NavDebugOverlay (same file, same
// shape): the host owns the debug-line buffer and supplies the ground height,
// everything else is shared.
//
// This is where the whole animation chain meets: the sim's CharacterAnim says
// what a character is doing, ClipFor/PhaseRatio say what that looks like, and
// the engine animation runtime poses the skeleton. Capsules remain the
// character blockout mesh -- this draws OVER them and is off by default.
//
// The per-character animator lives on the SIM entity (Sim::HandleForSlot), which
// is safe only because no sim system reads a render component; the invariant is
// pinned by a test in game/tests/determinism_tests.cpp.

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // Sim, CharacterState, CharacterAnim
#include "engine/animation/animation_set.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "game/visual/character_animation.hpp"  // LogicalClip

namespace badlands {

class SkeletonDebugOverlay {
 public:
  // Terrain height at a world XZ. Flat 0 for the sandbox arena, terrain height
  // for the game -- the same split NavDebugOverlay makes.
  using GroundHeightFn = std::function<float(float x, float z)>;

  // Loads the character assets. False (and the overlay stays permanently off)
  // when they are missing or broken -- a character asset problem must never stop
  // an app starting, so hosts log and carry on.
  bool Initialize(const std::string& manifest_path =
                      "assets/characters/quaternius/clips.json");

  // APPEND every visible character's posed skeleton into `out`. No-op when the
  // toggle is off or initialization failed.
  //
  // `rows` is the frame's EXISTING character snapshot, not a fresh one: both
  // hosts already take exactly one per frame and share it with their HUD and
  // picking, and a second snapshot could disagree with what was drawn.
  //
  // `anim_dt` is PRESENTATION time (stops when paused, scales with playback
  // speed) -- looping clips advance on it. Bounded actions ignore it entirely
  // and are re-derived from sim ticks, which is what keeps a swing locked to the
  // blow it depicts.
  void Rebuild(Sim& sim, std::span<const CharacterState> rows,
               DebugLineBuffer& out, const GroundHeightFn& ground_y,
               float anim_dt);

  // The ImGui widgets. The caller places them in its own window / header.
  void DrawControls();

  bool enabled() const { return show_ && ready_; }
  // The toggle itself, for a host that drives it from somewhere other than
  // DrawControls (the same shape as ConeOverlayPass::mutable_enabled).
  bool& mutable_enabled() { return show_; }

  // Characters drawn by the last Rebuild.
  int drawn() const { return drawn_; }

 private:
  // Scratch reused across characters within a frame. Poses are transient by
  // design (see pose.hpp), so the only persistent per-character cost is the
  // CharacterAnimator component.
  struct Scratch {
    std::optional<Pose> current;
    std::optional<Pose> fade_from;
    std::optional<Pose> blended;
    ClipSampler sampler_a;
    ClipSampler sampler_b;
  };

  // A logical clip's manifest index, resolved once at load. -1 when absent.
  int IndexOf(LogicalClip clip) const { return clip_index_[static_cast<int>(clip)]; }

  std::optional<AnimationSet> assets_;
  Scratch scratch_;
  bool ready_ = false;
  bool show_ = false;
  // Manifest index per logical clip, resolved once so nothing looks up a clip by
  // string per character per frame. -1 when the manifest lacks that clip.
  std::vector<int> clip_index_;
  int drawn_ = 0;  // characters drawn last frame, for the panel readout
};

}  // namespace badlands
