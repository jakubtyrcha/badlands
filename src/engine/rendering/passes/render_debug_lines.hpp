#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <dawn/webgpu_cpp.h>

namespace badlands {

class DebugLineBuffer;

// CPU-expands each world-space debug line into a screen-aligned quad (6 verts,
// kThickLine layout) and (re)creates `out_buffer` with the result. `view`/`proj`
// are the camera-OFFSET frame matrices (camera at origin) and `camera_world_pos`
// the world camera position — endpoints are offset by -camera_world_pos for the
// projection math and unprojected back to WORLD space, so the buffer holds world
// coords and thick_line.wesl's `worldSpaceToClipSpace` applies the single offset.
// Returns the vertex count (0 if there are no lines; `out_buffer` is cleared).
uint32_t UploadDebugLines(wgpu::Device device, wgpu::Buffer& out_buffer,
                          const DebugLineBuffer& lines, const glm::mat4& view,
                          const glm::mat4& proj, glm::vec2 screen_size,
                          glm::vec3 camera_world_pos);

}  // namespace badlands
