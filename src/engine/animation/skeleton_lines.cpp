#include "engine/animation/skeleton_lines.hpp"

#include <ozz/animation/runtime/skeleton.h>

#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"  // ToMat4
#include "engine/animation/skeleton.hpp"
#include "engine/rendering/debug_line_buffer.hpp"

namespace badlands {

void EmitSkeletonLines(const Skeleton& skeleton, const Pose& pose,
                       const glm::mat4& world, DebugLineBuffer& out,
                       glm::vec3 color, float thickness) {
  const ozz::span<const int16_t> parents = skeleton.joint_parents();
  const ozz::span<const ozz::math::Float4x4> models = pose.models();
  // A pose sized against a different skeleton would index past the joint
  // matrices; draw nothing rather than read out of bounds.
  if (models.size() < parents.size()) return;

  for (size_t joint = 0; joint < parents.size(); ++joint) {
    const int16_t parent = parents[joint];
    if (parent == ozz::animation::Skeleton::kNoParent) continue;

    // Column 3 of a model matrix is the joint's origin.
    const glm::vec3 from(world * ToMat4(models[static_cast<size_t>(parent)])[3]);
    const glm::vec3 to(world * ToMat4(models[joint])[3]);
    out.AddLine(from, to, color, thickness);
  }
}

}  // namespace badlands
