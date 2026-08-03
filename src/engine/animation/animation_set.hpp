#pragma once

// A skeleton plus its clips, named by a JSON manifest.
//
// The manifest maps a LOGICAL name to a file, so renaming or reauthoring a clip
// is a data change. It stays game-agnostic: this knows a clip is called "walk",
// not that walking is what a character does below some speed.
//
// Manifest shape (see assets/characters/quaternius/clips.json):
//   { "skeleton": "skeleton.ozz",
//     "clips": { "walk": "Rig_Walk_Loop.ozz",
//                "attack": { "file": "Rig_Sword_Attack.ozz", "pivot": 0.45 } } }
// A clip is either a bare filename or an object carrying that filename plus
// metadata. Paths are relative to the manifest's own directory. Keys starting
// with '_' are ignored, which is how the shipped manifest carries its comments.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/animation/animation_clip.hpp"
#include "engine/animation/skeleton.hpp"

namespace badlands {

class AnimationSet {
 public:
  // Loads the manifest and everything it names. nullopt if the manifest is
  // unreadable or its skeleton fails to load; a clip that fails to load is
  // logged and SKIPPED, so one bad file costs one animation rather than the
  // whole character.
  static std::optional<AnimationSet> Load(const std::string& manifest_path);

  AnimationSet(AnimationSet&&) = default;
  AnimationSet& operator=(AnimationSet&&) = default;
  AnimationSet(const AnimationSet&) = delete;
  AnimationSet& operator=(const AnimationSet&) = delete;

  const Skeleton& skeleton() const { return *skeleton_; }

  // Rotation about +Y, in radians, that turns the RIG'S OWN forward axis onto
  // +Z. Authored as "yaw_offset_degrees" beside "skeleton"; 0 when unstated.
  //
  // A rig faces whichever way its author modelled it, and that is rarely the
  // convention a game places characters with. This is where the two are
  // reconciled, ONCE per rig, rather than by a rotation baked into every
  // consumer -- swapping in a rig that faces the other way is then a data edit.
  float yaw_offset_radians() const { return yaw_offset_radians_; }

  int clip_count() const { return static_cast<int>(clips_.size()); }
  // Logical names in load order — what a viewer lists and what an index means.
  const std::string& clip_name(int index) const { return clips_[index].name; }
  const AnimationClip& clip(int index) const { return *clips_[index].clip; }

  // The clip's authored PIVOT: the normalized point at which its action
  // culminates, for a caller that must split one clip across two phases (a
  // swing's wind-up and its recovery meet exactly here).
  //
  // Defaults to 1.0 when the manifest does not say, which degrades to "the
  // whole clip is the first phase" rather than to anything broken. The engine
  // knows only that a clip has such a point; what the phases MEAN is the
  // caller's business.
  float clip_pivot(int index) const { return clips_[index].pivot; }

  // Index of a logical name, or -1. Callers resolve names to indices ONCE and
  // then work in indices; nothing samples by string per frame.
  int FindClip(const std::string& name) const;

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<AnimationClip> clip;
    float pivot = 1.0f;
  };

  AnimationSet() = default;
  std::unique_ptr<Skeleton> skeleton_;
  float yaw_offset_radians_ = 0.0f;
  std::vector<Entry> clips_;
};

}  // namespace badlands
