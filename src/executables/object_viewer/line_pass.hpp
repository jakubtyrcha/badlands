#pragma once

// The debug-line pass: world-space segments, expanded on the CPU into
// screen-aligned antialiased quads, drawn through the render graph.
//
// PORTED, NOT REWRITTEN. `DebugLineBuffer` and `ExpandDebugLines` come across
// from the Dawn renderer untouched -- they are pure CPU, already unit-tested
// (debug_line_expand_tests), and the near-plane clipping in there is subtle
// enough that reimplementing it would be a step backwards. What changed is only
// how the result reaches the GPU:
//
//   Dawn   ExpandDebugLines -> a fresh wgpu::Buffer per frame, vertex-buffer
//          bound, thick_line.wesl with a vertex input layout
//   RHI    ExpandDebugLines -> a FrameAllocator slice, pulled by SV_VertexID,
//          thick_line.slang with no vertex layout at all
//
// passes/render_debug_lines.cpp is deliberately NOT ported: it was a five-line
// wrapper that created a buffer per frame, and the frame allocator is what
// replaces it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/graph/render_graph.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::object_viewer {

// Mirrors LineFrame in shaders/slang/object_viewer/thick_line.slang. All-vec4
// members: MSL pads a float3 to 16 bytes, so a struct that looks packed here
// arrives strided on the GPU -- a mismatch that shows up as wrong geometry
// rather than as an error.
struct LineFrameUniform {
  glm::mat4 view_proj{1.0f};
  glm::vec4 camera_pos{0.0f};
};
static_assert(sizeof(LineFrameUniform) == 80);

class LinePass {
 public:
  // Returns null (after logging) if the shaders or pipeline cannot be built.
  // `target_format` must match the attachment the pass renders into.
  static std::unique_ptr<LinePass> Create(rhi::IRhiDevice& device,
                                          slang::SlangCompiler& compiler,
                                          rhi::Format target_format);

  // Recycles this frame's allocator slot. Call once per frame, after
  // IRhiDevice::BeginFrame.
  void BeginFrame(uint64_t frame_index);

  // Expands `lines` and uploads them. `view`/`proj` are the camera-OFFSET
  // matrices (camera at origin) that ExpandDebugLines documents; the shader
  // applies the single camera offset. Returns false, after logging, if the
  // upload could not be made -- which a caller must not treat as "no lines".
  bool Upload(const DebugLineBuffer& lines, const glm::mat4& view,
              const glm::mat4& proj, glm::vec2 screen_size,
              glm::vec3 camera_world_pos);

  // Adds the pass, unless the last Upload produced nothing. Returns false when
  // there is nothing to draw, so a caller can tell "no lines" from "failed".
  bool AddToGraph(graph::RenderGraph& graph, graph::ResourceHandle target,
                  rhi::LoadOp load);

  uint32_t VertexCount() const { return vertex_count_; }

 private:
  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  std::unique_ptr<rhi::FrameAllocator> alloc_;
  rhi::BindingTablePtr table_;

  std::vector<float> expanded_;
  uint32_t vertex_count_ = 0;
  uint32_t frame_offset_ = 0;
  uint32_t vertex_offset_ = 0;
};

}  // namespace badlands::object_viewer
