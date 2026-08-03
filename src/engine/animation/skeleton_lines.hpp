#pragma once

// Draws a posed skeleton as debug line segments: one segment per non-root
// joint, from its parent's model-space origin to its own.
//
// This is the whole presentation of a skeleton until characters have geometry,
// and it stays useful afterwards as an overlay. It APPENDS to the buffer rather
// than owning one, because a frame's debug lines come from several sources and
// SceneContext::debug_lines only points at one buffer.

#include <glm/glm.hpp>

namespace badlands {

class Pose;
class Skeleton;
class DebugLineBuffer;

// Appends `skeleton`'s bones, posed by `pose`, transformed by `world`.
// `pose.models()` must already be filled — call LocalToModel first.
// A root joint contributes no segment (it has no parent to draw from), so the
// output grows by exactly num_joints() - num_roots() lines.
void EmitSkeletonLines(const Skeleton& skeleton, const Pose& pose,
                       const glm::mat4& world, DebugLineBuffer& out,
                       glm::vec3 color = glm::vec3(1.0f, 1.0f, 0.0f),
                       float thickness = 2.0f);

}  // namespace badlands
