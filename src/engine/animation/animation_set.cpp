#include "engine/animation/animation_set.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <glm/trigonometric.hpp>  // glm::radians
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"  // ToMat4

namespace badlands {
namespace {

// Shared empty name for every out-of-range accessor, so they can all return a
// reference without each holding its own static.
const std::string& EmptyName() {
  static const std::string kEmpty;
  return kEmpty;
}

// An authored ratio is data, so it is clamped rather than trusted: a nonsense
// value would otherwise reach a sampler as an out-of-range time.
float ClampRatio(float value) { return std::clamp(value, 0.0f, 1.0f); }

// Reads a 16-float column-major matrix (glm::value_ptr order). nullopt when the
// array is the wrong length or holds a non-number, so a malformed socket is
// skipped instead of silently becoming a garbage transform.
std::optional<glm::mat4> ParseMat4(const nlohmann::ordered_json& value) {
  if (!value.is_array() || value.size() != 16) return std::nullopt;
  glm::mat4 m(1.0f);
  for (int i = 0; i < 16; ++i) {
    if (!value[static_cast<size_t>(i)].is_number()) return std::nullopt;
    m[i / 4][i % 4] = value[static_cast<size_t>(i)].get<float>();
  }
  return m;
}

}  // namespace

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

  // Descriptive only: which rig family this skeleton came from. Nothing here
  // behaves differently per family; it exists so a viewer can say what it loaded.
  if (const auto family_it = manifest.find("family");
      family_it != manifest.end() && family_it->is_string()) {
    set.family_ = family_it->get<std::string>();
  }

  // Rig orientation: how far to turn this skeleton so its own forward axis
  // lands on +Z. Optional; absent means the rig already faces +Z.
  if (const auto yaw_it = manifest.find("yaw_offset_degrees");
      yaw_it != manifest.end() && yaw_it->is_number()) {
    set.yaw_offset_radians_ = glm::radians(yaw_it->get<float>());
  }

  // Sockets are read but NOT resolved here: their parents are joint names, and
  // resolution happens once in BuildAttachments where the collision rule lives.
  std::vector<SocketDecl> sockets;
  if (const auto sockets_it = manifest.find("sockets");
      sockets_it != manifest.end()) {
    if (!sockets_it->is_array()) {
      spdlog::warn("AnimationSet::Load: {} has a \"sockets\" that is not an array; ignored",
                   manifest_path);
    } else {
      for (const auto& value : *sockets_it) {
        if (!value.is_object()) continue;
        const auto name_it = value.find("name");
        const auto parent_it = value.find("parent");
        const auto offset_it = value.find("offset");
        if (name_it == value.end() || !name_it->is_string() ||
            parent_it == value.end() || !parent_it->is_string()) {
          spdlog::warn("AnimationSet::Load: {} has a socket without a \"name\"/\"parent\"; skipped",
                       manifest_path);
          continue;
        }
        SocketDecl decl;
        decl.name = name_it->get<std::string>();
        decl.parent = parent_it->get<std::string>();
        // An absent offset means the socket sits exactly on its parent joint,
        // which is the common case for a prop point authored on a real bone.
        if (offset_it != value.end()) {
          std::optional<glm::mat4> offset = ParseMat4(*offset_it);
          if (!offset) {
            spdlog::warn(
                "AnimationSet::Load: socket \"{}\" has a malformed \"offset\" "
                "(expected 16 numbers); skipped",
                decl.name);
            continue;
          }
          decl.offset = *offset;
        }
        sockets.push_back(std::move(decl));
      }
    }
  }

  const auto clips_it = manifest.find("clips");
  if (clips_it == manifest.end() || !clips_it->is_object()) {
    spdlog::error("AnimationSet::Load: {} has no \"clips\" object", manifest_path);
    return std::nullopt;
  }

  for (const auto& [name, value] : clips_it->items()) {
    if (!name.empty() && name.front() == '_') continue;  // comment key

    // A clip is either a bare filename or an object carrying one plus metadata.
    // Both forms stay valid so adding a marker to one clip does not require
    // rewriting every other entry.
    std::string file;
    std::vector<Marker> markers;
    if (value.is_string()) {
      file = value.get<std::string>();
    } else if (value.is_object()) {
      const auto file_it = value.find("file");
      if (file_it == value.end() || !file_it->is_string()) {
        spdlog::warn("AnimationSet::Load: clip \"{}\" has no \"file\"; skipped", name);
        continue;
      }
      file = file_it->get<std::string>();

      // "pivot" predates the marker table and stays valid: it is simply the
      // marker named "pivot". Read FIRST so an explicit markers entry of the
      // same name overrides it rather than colliding with it.
      const auto pivot_it = value.find("pivot");
      const bool has_pivot = pivot_it != value.end() && pivot_it->is_number();
      if (has_pivot) {
        markers.push_back(Marker{"pivot", ClampRatio(pivot_it->get<float>())});
      }

      if (const auto markers_it = value.find("markers");
          markers_it != value.end() && markers_it->is_object()) {
        for (const auto& [marker_name, marker_value] : markers_it->items()) {
          if (!marker_value.is_number()) {
            spdlog::warn("AnimationSet::Load: clip \"{}\" marker \"{}\" is not a number; skipped",
                         name, marker_name);
            continue;
          }
          const float ratio = ClampRatio(marker_value.get<float>());
          const auto existing =
              std::find_if(markers.begin(), markers.end(),
                           [&](const Marker& m) { return m.name == marker_name; });
          if (existing != markers.end()) {
            // Only reachable for "pivot", authored both ways. The table wins,
            // because it is the form that can carry every other marker too.
            spdlog::warn(
                "AnimationSet::Load: clip \"{}\" declares \"{}\" both as a field "
                "and in \"markers\"; using the \"markers\" value",
                name, marker_name);
            existing->ratio = ratio;
            continue;
          }
          markers.push_back(Marker{marker_name, ratio});
        }
      }
    } else {
      spdlog::warn("AnimationSet::Load: clip \"{}\" is neither a filename nor an object; skipped",
                   name);
      continue;
    }

    std::optional<AnimationClip> clip = AnimationClip::Load((dir / file).string());
    // One unreadable clip costs one animation, not the character: a viewer can
    // still show everything else, which is more useful than refusing to start.
    if (!clip) continue;
    set.clips_.push_back(Entry{name, std::make_unique<AnimationClip>(std::move(*clip)),
                               std::move(markers)});
  }

  if (set.clips_.empty()) {
    spdlog::error("AnimationSet::Load: {} loaded no clips", manifest_path);
    return std::nullopt;
  }

  set.BuildAttachments(sockets, manifest_path);

  spdlog::info(
      "AnimationSet: {} -- {} joints, {} clips, {} attachments, rig yaw offset {:.0f} deg",
      manifest_path, set.skeleton_->num_joints(), set.clips_.size(),
      set.attachments_.size(), glm::degrees(set.yaw_offset_radians_));
  return set;
}

void AnimationSet::BuildAttachments(const std::vector<SocketDecl>& sockets,
                                    const std::string& manifest_path) {
  const ozz::span<const char* const> joint_names = skeleton_->joint_names();

  // Joints FIRST, in skeleton order, so an attachment id below num_joints() is
  // that joint's index. Callers are told they may rely on this.
  attachments_.reserve(joint_names.size() + sockets.size());
  for (size_t joint = 0; joint < joint_names.size(); ++joint) {
    const char* joint_name = joint_names[joint];
    Attachment attachment;
    attachment.name = joint_name != nullptr ? joint_name : std::string();
    attachment.joint = static_cast<int>(joint);
    attachment.offset = glm::mat4(1.0f);
    // A skeleton with two identically named joints is malformed data, and the
    // first one wins -- but say so, because the second becomes unaddressable.
    if (!attachment_index_.emplace(attachment.name,
                                   static_cast<int>(attachments_.size())).second) {
      spdlog::warn("AnimationSet::Load: {} skeleton has two joints named \"{}\"; "
                   "the second is not addressable by name",
                   manifest_path, attachment.name);
    }
    attachments_.push_back(std::move(attachment));
  }

  for (const SocketDecl& decl : sockets) {
    // A socket parent is always a JOINT, and joints are exactly the ids below
    // num_joints. Sockets already added in this loop are addressable by name, so
    // without that bound a socket could parent to another socket -- which the
    // manifest cannot express and which no consumer expects.
    const auto parent = attachment_index_.find(decl.parent);
    if (parent == attachment_index_.end() ||
        parent->second >= static_cast<int>(joint_names.size())) {
      spdlog::warn("AnimationSet::Load: socket \"{}\" names unknown parent joint \"{}\"; skipped",
                   decl.name, decl.parent);
      continue;
    }
    const int parent_joint = parent->second;

    // JOINTS WIN. A socket sharing a joint's name is a duplicate of it -- 0 A.D.
    // carries both `weapon_R` and `prop-weapon_R` a millimetre apart -- and the
    // joint is the animated one. A packer should already have dropped this, so
    // reaching it means the manifest was hand-edited or packed by an older tool.
    if (attachment_index_.count(decl.name) != 0) {
      spdlog::warn("AnimationSet::Load: socket \"{}\" collides with an existing "
                   "attachment; dropped (joints win)",
                   decl.name);
      continue;
    }

    Attachment attachment;
    attachment.name = decl.name;
    attachment.joint = parent_joint;
    attachment.offset = decl.offset;
    attachment_index_.emplace(attachment.name,
                              static_cast<int>(attachments_.size()));
    attachments_.push_back(std::move(attachment));
  }
}

const std::string& AnimationSet::attachment_name(int id) const {
  if (id < 0 || id >= static_cast<int>(attachments_.size())) return EmptyName();
  return attachments_[static_cast<size_t>(id)].name;
}

int AnimationSet::FindAttachment(const std::string& name) const {
  const auto it = attachment_index_.find(name);
  return it != attachment_index_.end() ? it->second : -1;
}

glm::mat4 AnimationSet::AttachmentTransform(int id, const Pose& pose) const {
  if (id < 0 || id >= static_cast<int>(attachments_.size())) return glm::mat4(1.0f);
  const Attachment& attachment = attachments_[static_cast<size_t>(id)];

  const ozz::span<const ozz::math::Float4x4> models = pose.models();
  // A pose sized against a different skeleton would index past the joint
  // matrices; return identity rather than read out of bounds.
  if (attachment.joint < 0 ||
      static_cast<size_t>(attachment.joint) >= models.size()) {
    return glm::mat4(1.0f);
  }

  // PARENT THEN OFFSET. Reversing these attaches the item to the right place
  // pointing the wrong way, which reads as a rig bug rather than a math one.
  return ToMat4(models[static_cast<size_t>(attachment.joint)]) * attachment.offset;
}

std::optional<float> AnimationSet::clip_marker(int index,
                                              const std::string& name) const {
  if (!ValidClip(index)) return std::nullopt;
  for (const Marker& marker : clips_[static_cast<size_t>(index)].markers) {
    if (marker.name == name) return marker.ratio;
  }
  return std::nullopt;
}

int AnimationSet::clip_marker_count(int index) const {
  if (!ValidClip(index)) return 0;
  return static_cast<int>(clips_[static_cast<size_t>(index)].markers.size());
}

const std::string& AnimationSet::clip_marker_name(int index, int marker) const {
  if (!ValidClip(index)) return EmptyName();
  const std::vector<Marker>& markers = clips_[static_cast<size_t>(index)].markers;
  if (marker < 0 || marker >= static_cast<int>(markers.size())) return EmptyName();
  return markers[static_cast<size_t>(marker)].name;
}

float AnimationSet::clip_marker_value(int index, int marker) const {
  if (!ValidClip(index)) return 0.0f;
  const std::vector<Marker>& markers = clips_[static_cast<size_t>(index)].markers;
  if (marker < 0 || marker >= static_cast<int>(markers.size())) return 0.0f;
  return markers[static_cast<size_t>(marker)].ratio;
}

float AnimationSet::clip_pivot(int index) const {
  return clip_marker(index, "pivot").value_or(1.0f);
}

int AnimationSet::FindClip(const std::string& name) const {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (clips_[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace badlands
