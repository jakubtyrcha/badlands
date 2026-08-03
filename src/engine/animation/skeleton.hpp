#pragma once

// A loaded ozz skeleton: the joint hierarchy a Pose is sized against and a
// clip is sampled onto. Game-agnostic — nothing here knows about characters,
// EnTT or badlands, which is what lets badlands_viewer use it with no Sim.
//
// Loading is from a .ozz archive produced by ozz's offline pipeline (we ship
// pre-converted data under assets/characters/, so no importer is built).

#include <optional>
#include <string>

#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/span.h>

namespace badlands {

class Skeleton {
 public:
  // Reads a skeleton from a .ozz archive. nullopt when the file is missing,
  // unreadable, or does not contain a skeleton — a wrong-but-valid archive is
  // a content error, so it is reported, not asserted.
  static std::optional<Skeleton> Load(const std::string& path);

  Skeleton(Skeleton&&) = default;
  Skeleton& operator=(Skeleton&&) = default;
  Skeleton(const Skeleton&) = delete;
  Skeleton& operator=(const Skeleton&) = delete;

  int num_joints() const { return skeleton_.num_joints(); }
  // SoA width: a Pose's local buffer is sized in these, not in joints.
  int num_soa_joints() const { return skeleton_.num_soa_joints(); }

  // Parent index per joint, or Skeleton::kNoParent for a root. Joints are
  // ordered parents-before-children, which is what lets the line emitter walk
  // them in one pass.
  ozz::span<const int16_t> joint_parents() const { return skeleton_.joint_parents(); }
  ozz::span<const char* const> joint_names() const { return skeleton_.joint_names(); }

  // How many joints have no parent. The skeleton-line emitter draws one segment
  // per NON-root joint, so this is the count its output is short by.
  int num_roots() const;

  const ozz::animation::Skeleton& raw() const { return skeleton_; }

 private:
  Skeleton() = default;
  ozz::animation::Skeleton skeleton_;
};

}  // namespace badlands
