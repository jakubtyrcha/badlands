#pragma once

#include <vector>

#include <glm/glm.hpp>

namespace badlands {

class DebugLineBuffer;

// Pure CPU expansion of world-space debug segments into screen-aligned,
// antialiased thick-line quad vertices (6 verts/segment, kThickLine layout: 8
// floats each = pos(3) + color(3) + signed-dist(1) + half_thickness(1)). No GPU
// dependency, so it is unit-testable on its own (see debug_line_expand_tests).
//
// `view`/`proj` are the camera-OFFSET frame matrices (camera at origin);
// `camera_world_pos` is the world camera position. Each endpoint is rebased by
// -camera_world_pos for the projection math and the emitted quad corners are
// unprojected back to WORLD space (so thick_line.wesl's `worldSpaceToClipSpace`
// applies the single camera offset).
//
// Segments crossing behind the camera are near-plane clipped in clip space (to
// w >= epsilon) BEFORE the perspective divide, so no vertex is ever produced
// from a division by a non-positive w (which would streak garbage across the
// screen). A segment with BOTH endpoints behind the camera collapses to a
// zero-area (degenerate) quad that rasterizes to nothing.
//
// Returns a flat float buffer of size (segment_count * 6 * 8); empty when there
// are no lines.
std::vector<float> ExpandDebugLines(const DebugLineBuffer& lines,
                                    const glm::mat4& view, const glm::mat4& proj,
                                    glm::vec2 screen_size,
                                    glm::vec3 camera_world_pos);

}  // namespace badlands
