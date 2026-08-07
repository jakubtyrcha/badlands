#include "engine/animation/attachment_lines.hpp"

#include "engine/animation/animation_set.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/rendering/debug_line_buffer.hpp"

namespace badlands {
namespace {

// The conventional axis colouring: X red, Y green, Z blue. Anything else makes
// a rig read wrong against every other tool a modeller uses.
constexpr glm::vec3 kAxisColors[3] = {
    {1.0f, 0.2f, 0.2f},
    {0.2f, 1.0f, 0.2f},
    {0.3f, 0.4f, 1.0f},
};

}  // namespace

void EmitAttachmentAxes(const AnimationSet& set, const Pose& pose,
                        const glm::mat4& world, DebugLineBuffer& out,
                        float length, float thickness) {
  // A pose sized against a different skeleton would index past the joint
  // matrices; draw nothing rather than read out of bounds. Checked ONCE here so
  // AttachmentTransform's own guard never silently turns half the rig into
  // triads stacked at the origin.
  if (pose.models().size() < static_cast<size_t>(set.skeleton().num_joints())) {
    return;
  }

  for (int id = 0; id < set.attachment_count(); ++id) {
    const glm::mat4 m = world * set.AttachmentTransform(id, pose);
    const glm::vec3 origin(m[3]);
    // Basis columns are NOT normalized: an attachment carrying a scale should
    // look wrong here rather than be quietly hidden.
    for (int axis = 0; axis < 3; ++axis) {
      out.AddLine(origin, origin + length * glm::vec3(m[axis]),
                  kAxisColors[axis], thickness);
    }
  }
}

}  // namespace badlands
