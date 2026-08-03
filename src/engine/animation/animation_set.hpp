#pragma once

// A skeleton plus its clips, named by a JSON manifest.
//
// The manifest maps a LOGICAL name to a file, so renaming or reauthoring a clip
// is a data change. It stays game-agnostic: this knows a clip is called "walk",
// not that walking is what a character does below some speed.
//
// Manifest shape (see assets/characters/quaternius/clips.json):
//   { "skeleton": "skeleton.ozz", "clips": { "walk": "Rig_Walk_Loop.ozz", ... } }
// Paths are relative to the manifest's own directory. Keys starting with '_'
// are ignored, which is how the shipped manifest carries its comment block.

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

  int clip_count() const { return static_cast<int>(clips_.size()); }
  // Logical names in load order — what a viewer lists and what an index means.
  const std::string& clip_name(int index) const { return clips_[index].name; }
  const AnimationClip& clip(int index) const { return *clips_[index].clip; }

  // Index of a logical name, or -1. Callers resolve names to indices ONCE and
  // then work in indices; nothing samples by string per frame.
  int FindClip(const std::string& name) const;

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<AnimationClip> clip;
  };

  AnimationSet() = default;
  std::unique_ptr<Skeleton> skeleton_;
  std::vector<Entry> clips_;
};

}  // namespace badlands
