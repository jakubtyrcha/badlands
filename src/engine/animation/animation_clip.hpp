#pragma once

// One loaded ozz animation clip. Sampling is in the unit interval [0,1] rather
// than seconds, so a clip stretched to fit a gameplay window (an attack
// wind-up) and a clip played at its own rate (a walk cycle) use the same call.

#include <optional>
#include <string>

#include <ozz/animation/runtime/animation.h>

namespace badlands {

class AnimationClip {
 public:
  // Reads a clip from a .ozz archive. nullopt when the file is missing,
  // unreadable, or holds something other than an animation.
  static std::optional<AnimationClip> Load(const std::string& path);

  AnimationClip(AnimationClip&&) = default;
  AnimationClip& operator=(AnimationClip&&) = default;
  AnimationClip(const AnimationClip&) = delete;
  AnimationClip& operator=(const AnimationClip&) = delete;

  float duration_seconds() const { return animation_.duration(); }
  int num_tracks() const { return animation_.num_tracks(); }
  // The clip's own authored name, which need not match the file or the logical
  // name an AnimationSet filed it under.
  const char* name() const { return animation_.name(); }

  const ozz::animation::Animation& raw() const { return animation_; }

 private:
  AnimationClip() = default;
  ozz::animation::Animation animation_;
};

}  // namespace badlands
