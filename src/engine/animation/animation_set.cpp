#include "engine/animation/animation_set.hpp"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

std::optional<AnimationSet> AnimationSet::Load(const std::string& manifest_path) {
  std::ifstream stream(manifest_path);
  if (!stream) {
    spdlog::error("AnimationSet::Load: cannot open {}", manifest_path);
    return std::nullopt;
  }

  // ordered_json, NOT json: the default is std::map-backed, so iterating its
  // "clips" object would yield keys ALPHABETICALLY and clip indices would not
  // match the manifest's authored order (index 0 would be "attack", not
  // "idle"). clip_name(index) promises load order, so the parse must preserve it.
  nlohmann::ordered_json manifest;
  try {
    stream >> manifest;
  } catch (const nlohmann::json::exception& e) {
    spdlog::error("AnimationSet::Load: {} is not valid JSON: {}", manifest_path,
                  e.what());
    return std::nullopt;
  }

  const std::filesystem::path dir =
      std::filesystem::path(manifest_path).parent_path();

  const auto skeleton_it = manifest.find("skeleton");
  if (skeleton_it == manifest.end() || !skeleton_it->is_string()) {
    spdlog::error("AnimationSet::Load: {} has no \"skeleton\" entry", manifest_path);
    return std::nullopt;
  }

  AnimationSet set;
  const std::string skeleton_path =
      (dir / skeleton_it->get<std::string>()).string();
  std::optional<Skeleton> skeleton = Skeleton::Load(skeleton_path);
  if (!skeleton) return std::nullopt;  // Skeleton::Load already logged why
  set.skeleton_ = std::make_unique<Skeleton>(std::move(*skeleton));

  const auto clips_it = manifest.find("clips");
  if (clips_it == manifest.end() || !clips_it->is_object()) {
    spdlog::error("AnimationSet::Load: {} has no \"clips\" object", manifest_path);
    return std::nullopt;
  }

  for (const auto& [name, value] : clips_it->items()) {
    if (!name.empty() && name.front() == '_') continue;  // comment key
    if (!value.is_string()) {
      spdlog::warn("AnimationSet::Load: clip \"{}\" is not a filename; skipped", name);
      continue;
    }
    const std::string clip_path = (dir / value.get<std::string>()).string();
    std::optional<AnimationClip> clip = AnimationClip::Load(clip_path);
    // One unreadable clip costs one animation, not the character: a viewer can
    // still show everything else, which is more useful than refusing to start.
    if (!clip) continue;
    set.clips_.push_back(
        Entry{name, std::make_unique<AnimationClip>(std::move(*clip))});
  }

  if (set.clips_.empty()) {
    spdlog::error("AnimationSet::Load: {} loaded no clips", manifest_path);
    return std::nullopt;
  }

  spdlog::info("AnimationSet: {} -- {} joints, {} clips", manifest_path,
               set.skeleton_->num_joints(), set.clips_.size());
  return set;
}

int AnimationSet::FindClip(const std::string& name) const {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (clips_[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace badlands
