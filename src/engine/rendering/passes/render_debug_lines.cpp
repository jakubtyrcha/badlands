#include "engine/rendering/passes/render_debug_lines.hpp"

#include <cstring>
#include <vector>

#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rendering/debug_line_expand.hpp"

namespace badlands {

uint32_t UploadDebugLines(wgpu::Device device, wgpu::Buffer& out_buffer,
                          const DebugLineBuffer& lines, const glm::mat4& view,
                          const glm::mat4& proj, glm::vec2 screen_size,
                          glm::vec3 camera_world_pos) {
  // CPU expansion (near-plane-clipped, world-space) lives in a GPU-free TU so it
  // can be unit-tested; this wrapper just uploads the result.
  const std::vector<float> data =
      ExpandDebugLines(lines, view, proj, screen_size, camera_world_pos);
  if (data.empty()) {
    out_buffer = nullptr;
    return 0;
  }

  const size_t size = data.size() * sizeof(float);
  wgpu::BufferDescriptor bd;
  bd.size = size;
  bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = true;
  out_buffer = device.CreateBuffer(&bd);
  std::memcpy(out_buffer.GetMappedRange(0, size), data.data(), size);
  out_buffer.Unmap();
  return static_cast<uint32_t>(data.size() / 8);
}

}  // namespace badlands
