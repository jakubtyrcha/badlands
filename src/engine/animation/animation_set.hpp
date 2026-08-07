#pragma once

// A skeleton plus its clips, named by a JSON manifest.
//
// The manifest maps a LOGICAL name to a file, so renaming or reauthoring a clip
// is a data change. It stays game-agnostic: this knows a clip is called "walk",
// not that walking is what a character does below some speed.
//
// Manifest shape (see assets/characters/quaternius/clips.json):
//   { "skeleton": "skeleton.ozz",
//     "family": "Biped",
//     "sockets": [ { "name": "backplate", "parent": "spine",
//                    "offset": [ 16 floats, column-major ] } ],
//     "clips": { "walk": "Rig_Walk_Loop.ozz",
//                "attack": { "file": "Rig_Sword_Attack.ozz",
//                            "markers": { "pivot": 0.45, "event": 0.5 } } } }
// A clip is either a bare filename or an object carrying that filename plus
// metadata. Paths are relative to the manifest's own directory. Keys starting
// with '_' are ignored, which is how the shipped manifest carries its comments.
//
// "family", "sockets" and "markers" are all optional, so a manifest predating
// them loads unchanged as a rig with no sockets and no markers.

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>

#include "engine/animation/animation_clip.hpp"
#include "engine/animation/skeleton.hpp"

namespace badlands {

class Pose;

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

  // The rig family this skeleton belongs to ("Biped", "Horse"), as authored.
  // Empty when the manifest does not say. Descriptive only — nothing here
  // behaves differently per family.
  const std::string& family() const { return family_; }

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

  // ---------------------------------------------------------------------------
  // Attachments: ONE namespace over joints and sockets.
  //
  // A skeleton joint and an authored socket are both addressed by name through
  // FindAttachment, and nothing here can ask which it found. That is the point:
  // whether a prop point is carried as an animated joint or collapsed into a
  // static socket is an IMPORT-TIME SIZE DECISION, not a contract. A socket that
  // later has to become a real joint again changes no caller.
  //
  // Every attachment is (joint index, offset), so a joint is simply one whose
  // offset is identity — there is no branch anywhere in the resolution.
  //
  // Ids [0, skeleton().num_joints()) ARE the joint indices, in skeleton order;
  // sockets follow. Callers may rely on that: it is what makes an attachment
  // transform on a joint exactly its model matrix.
  //
  // Names are unique across the union and JOINTS WIN. A socket colliding with a
  // joint name is dropped at load with a warning, because the two are duplicates
  // of one another and the joint is the animated one.
  // ---------------------------------------------------------------------------

  int attachment_count() const { return static_cast<int>(attachments_.size()); }
  // Empty string for an out-of-range id, so a caller iterating a stale count
  // gets nothing rather than reading past the end.
  const std::string& attachment_name(int id) const;
  // Index of a name, or -1. Resolve ONCE and work in ids; nothing looks up an
  // attachment by string per frame.
  int FindAttachment(const std::string& name) const;

  // Model-space transform of an attachment: the parent joint's model matrix
  // composed with the attachment's offset, in that order. `pose.models()` must
  // already be filled (call LocalToModel first).
  //
  // Identity for an out-of-range id or a pose sized against another skeleton —
  // the same degrade-don't-crash rule EmitSkeletonLines follows.
  glm::mat4 AttachmentTransform(int id, const Pose& pose) const;

  // ---------------------------------------------------------------------------
  // Markers: free-form named points in [0,1] on a clip.
  //
  // What a marker MEANS is the caller's business. The engine knows only that a
  // clip declares named instants; whether "event" is when damage lands or when a
  // sound plays is decided a layer up.
  // ---------------------------------------------------------------------------

  std::optional<float> clip_marker(int index, const std::string& name) const;
  int clip_marker_count(int index) const;
  // Authored order, which is what a debug panel lists.
  const std::string& clip_marker_name(int index, int marker) const;
  float clip_marker_value(int index, int marker) const;

  // The clip's authored PIVOT: the normalized point at which its action
  // culminates, for a caller that must split one clip across two phases (a
  // swing's wind-up and its recovery meet exactly here).
  //
  // This is the marker named "pivot", defaulting to 1.0 when the manifest does
  // not declare it — which degrades to "the whole clip is the first phase"
  // rather than to anything broken.
  float clip_pivot(int index) const;

  // Index of a logical name, or -1. Callers resolve names to indices ONCE and
  // then work in indices; nothing samples by string per frame.
  int FindClip(const std::string& name) const;

 private:
  struct Marker {
    std::string name;
    float ratio = 0.0f;
  };

  struct Entry {
    std::string name;
    std::unique_ptr<AnimationClip> clip;
    std::vector<Marker> markers;  // authored order
  };

  struct Attachment {
    std::string name;
    int joint = -1;               // index into the skeleton
    glm::mat4 offset{1.0f};       // identity for a joint attachment
  };

  // A socket exactly as the manifest declares it: its parent is a joint NAME,
  // not an index, because a rig packer's joint order need not be the skeleton's
  // and a name is checkable by eye.
  struct SocketDecl {
    std::string name;
    std::string parent;
    glm::mat4 offset{1.0f};
  };

  AnimationSet() = default;

  // Fills attachments_ with every joint (identity offset) and then every socket
  // the manifest declares, skipping the ones that collide or name an unknown
  // parent. Called once, at the end of Load.
  void BuildAttachments(const std::vector<SocketDecl>& sockets,
                        const std::string& manifest_path);

  bool ValidClip(int index) const {
    return index >= 0 && index < static_cast<int>(clips_.size());
  }

  std::unique_ptr<Skeleton> skeleton_;
  std::string family_;
  float yaw_offset_radians_ = 0.0f;
  std::vector<Entry> clips_;
  std::vector<Attachment> attachments_;
  std::unordered_map<std::string, int> attachment_index_;
};

}  // namespace badlands
