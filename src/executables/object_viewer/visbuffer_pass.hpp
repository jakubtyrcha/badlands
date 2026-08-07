#pragma once

// The visibility-buffer raster pass: geometry in, one uint per pixel out.
//
// The pass binds NO MATERIAL and samples NO TEXTURE. Everything the surface
// needs is recovered afterwards, in the resolve, from the packed id plus the
// depth buffer -- which is the property that makes geometry cost independent of
// material complexity, and the reason the visibility buffer is worth the
// reconstruction work at all.
//
// Its targets are owned here rather than by the graph, because they must
// survive between frames (the resolve reads them in the same frame, but the
// binding tables that name them are immutable and rebuilding one per frame is
// the record-path allocation rule 11 bans).

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "executables/object_viewer/mesh_types.hpp"

namespace badlands::object_viewer {

// Mirrors VisFrame in shaders/slang/object_viewer/visbuffer.slang. All-float4
// members: MSL pads a float3 to 16 bytes, so a struct that looks packed here
// arrives strided on the GPU.
struct VisFrameUniform {
  glm::mat4 view_proj{1.0f};
  glm::vec4 camera_pos{0.0f};
};
static_assert(sizeof(VisFrameUniform) == 80);

class VisbufferPass {
 public:
  // Returns null (after logging) if the shaders, the pipeline, the mesh buffers
  // or the targets cannot be built.
  static std::unique_ptr<VisbufferPass> Create(rhi::IRhiDevice& device,
                                               slang::SlangCompiler& compiler,
                                               const SceneMesh& mesh,
                                               uint32_t width, uint32_t height);

  // Rebuilds the targets for a new size. Returns false after logging; a caller
  // must not treat that as "keep the old ones", because they are already gone.
  bool Resize(uint32_t width, uint32_t height);

  // The camera for this frame. `view`/`proj` are camera-OFFSET matrices (the
  // camera at the origin, the world rebased by -position), matching what the
  // line pass already uses so both see one convention.
  void SetCamera(const glm::mat4& view, const glm::mat4& proj,
                 glm::vec3 camera_world_pos);

  // Recycles this frame's allocator slot. Call once per frame, after
  // IRhiDevice::BeginFrame -- and here rather than in AddToGraph, because a
  // SKIPPED frame still consumes its slot.
  void BeginFrame(uint64_t frame_index);

  // Adds the raster pass. Returns false after logging if the frame uniform
  // could not be uploaded -- which a caller must not read as "nothing to draw".
  bool AddToGraph(graph::RenderGraph& graph);

  // WHERE this frame's uniform landed. Exposed because the hazard it guards is
  // INVISIBLE in a rendered image until frames overlap, and then it shows as
  // intermittent smearing rather than as a failure -- so a test asserts that
  // consecutive frames get distinct offsets instead.
  uint32_t LastFrameOffset() const { return frame_offset_; }
  rhi::IBuffer* LastFrameBuffer() const { return frame_buffer_; }

  // The graph handles for this frame's resources, valid after AddToGraph.
  //
  // The INDEX and DRAW buffers are imported here even though this pass does not
  // read them as storage -- it draws with the index buffer and never touches
  // DrawInfo. The resolve needs both, and importing one buffer twice in a graph
  // would give it two independent state trackers and a redundant barrier.
  graph::ResourceHandle VisbufferHandle() const { return vis_handle_; }
  graph::ResourceHandle DepthHandle() const { return depth_handle_; }
  graph::ResourceHandle VerticesHandle() const { return vtx_handle_; }
  graph::ResourceHandle IndicesHandle() const { return idx_handle_; }
  graph::ResourceHandle DrawsHandle() const { return draw_handle_; }

  rhi::ITexture* Visbuffer() const { return visbuffer_.get(); }
  rhi::ITexture* Depth() const { return depth_.get(); }
  rhi::IBuffer* Vertices() const { return vertex_buffer_.get(); }
  rhi::IBuffer* Indices() const { return index_buffer_.get(); }
  rhi::IBuffer* Draws() const { return draw_buffer_.get(); }
  uint32_t IndexCount() const { return index_count_; }

  // R32Uint, cleared to kVisEmpty (0). Exposed because the resolve's pipeline
  // and any readback both have to agree with it.
  static constexpr rhi::Format kVisFormat = rhi::Format::R32Uint;
  static constexpr rhi::Format kDepthFormat = rhi::Format::Depth32Float;

 private:
  bool BuildTargets(uint32_t width, uint32_t height);
  // Rebuilt when a ring growth block moves the uniform to another buffer.
  bool BuildTable(rhi::IBuffer* frame_buffer);

  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;

  rhi::BufferPtr vertex_buffer_, index_buffer_, draw_buffer_;
  // THE UNIFORM COMES FROM A RING, not a single buffer.
  //
  // frames_in_flight is 3 and the shell paces to that without ever waiting for
  // idle, so a plain per-frame memcpy into one buffer overwrites bytes the GPU
  // is still reading two frames back. For the visibility buffer that is worse
  // than a stale frame: the raster and the resolve would project the SAME
  // triangle with DIFFERENT matrices, so the barycentrics are computed against
  // a triangle that is not where the pixel was rasterized, and the material
  // smears along every edge while the camera moves.
  std::unique_ptr<rhi::FrameAllocator> alloc_;
  rhi::BindingTablePtr table_;
  rhi::IBuffer* table_frame_buffer_ = nullptr;
  rhi::IBuffer* frame_buffer_ = nullptr;
  uint32_t frame_offset_ = 0;
  uint32_t index_count_ = 0;
  uint32_t instance_count_ = 1;

  rhi::TexturePtr visbuffer_, depth_;
  uint32_t width_ = 0, height_ = 0;

  VisFrameUniform frame_{};
  graph::ResourceHandle vis_handle_, depth_handle_;
  graph::ResourceHandle vtx_handle_, idx_handle_, draw_handle_;
};

}  // namespace badlands::object_viewer
