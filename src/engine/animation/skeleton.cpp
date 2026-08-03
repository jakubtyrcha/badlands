#include "engine/animation/skeleton.hpp"

#include <spdlog/spdlog.h>

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

namespace badlands {

std::optional<Skeleton> Skeleton::Load(const std::string& path) {
  ozz::io::File file(path.c_str(), "rb");
  if (!file.opened()) {
    spdlog::error("Skeleton::Load: cannot open {}", path);
    return std::nullopt;
  }

  ozz::io::IArchive archive(&file);
  // ozz tags its high-level types, so a .ozz holding an ANIMATION (an easy
  // mistake when a manifest names the wrong file) is caught here rather than
  // deserializing into garbage.
  if (!archive.TestTag<ozz::animation::Skeleton>()) {
    spdlog::error("Skeleton::Load: {} does not contain a skeleton", path);
    return std::nullopt;
  }

  Skeleton result;
  archive >> result.skeleton_;
  return result;
}

int Skeleton::num_roots() const {
  int roots = 0;
  for (int16_t parent : skeleton_.joint_parents()) {
    if (parent == ozz::animation::Skeleton::kNoParent) {
      ++roots;
    }
  }
  return roots;
}

}  // namespace badlands
