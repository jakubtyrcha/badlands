#pragma once

// Draws a rig's ATTACHMENTS as debug axis triads: three coloured segments at
// each named point a caller can attach something to.
//
// Attachments are joints and sockets in one namespace (see AnimationSet), so
// this draws both and distinguishes neither -- which is the point. What you see
// is exactly what FindAttachment can address, and a socket that later collapses
// into a joint keeps its triad and its name.
//
// Like EmitSkeletonLines it APPENDS to the buffer rather than owning one,
// because a frame's debug lines come from several sources and
// SceneContext::debug_lines only points at one buffer.

#include <glm/glm.hpp>

namespace badlands {

class AnimationSet;
class DebugLineBuffer;
class Pose;

// Length of each axis arm, in rig units. Sized for a ~1.5-2 m humanoid carrying
// 50-80 attachments: long enough to read an orientation, short enough that the
// triads do not merge into a cloud. A constant, deliberately -- there is no
// control for it.
inline constexpr float kAttachmentAxisLength = 0.05f;

// Appends an RGB axis triad at each of `set`'s attachments, posed by `pose` and
// transformed by `world`. `pose.models()` must already be filled -- call
// LocalToModel first.
//
// X is red, Y green, Z blue, each `length` long in rig units.
// Output grows by exactly 3 * set.attachment_count() lines -- or by nothing at
// all, when `pose` is sized against a different skeleton.
void EmitAttachmentAxes(const AnimationSet& set, const Pose& pose,
                        const glm::mat4& world, DebugLineBuffer& out,
                        float length = kAttachmentAxisLength,
                        float thickness = 2.0f);

}  // namespace badlands
