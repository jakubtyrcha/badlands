#include "engine/animation/animation_clip.hpp"

#include <spdlog/spdlog.h>

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

namespace badlands {

std::optional<AnimationClip> AnimationClip::Load(const std::string& path) {
  ozz::io::File file(path.c_str(), "rb");
  if (!file.opened()) {
    spdlog::error("AnimationClip::Load: cannot open {}", path);
    return std::nullopt;
  }

  ozz::io::IArchive archive(&file);
  if (!archive.TestTag<ozz::animation::Animation>()) {
    spdlog::error("AnimationClip::Load: {} does not contain an animation", path);
    return std::nullopt;
  }

  AnimationClip result;
  archive >> result.animation_;
  return result;
}

}  // namespace badlands
