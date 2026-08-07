#pragma once

// The visibility-buffer resolve, and the debug views it can substitute for the
// lit result.
//
// One pipeline with a uniform branch at the end, not ten pipelines. That is
// what makes a debug view trustworthy: `Lit` and `Roughness` run the SAME
// fetch, the SAME barycentrics and the SAME material sampling, so a preview
// describes what the lit path actually consumed rather than what a parallel
// code path recomputed.

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include <glm/glm.hpp>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "executables/object_viewer/material_pack.hpp"

namespace badlands::object_viewer {

// Mirrors the kView* constants in shaders/slang/object_viewer/resolve.slang.
//
// EVERY ENTRY IS REACHABLE FROM --debug-view AS WELL AS FROM THE UI, and a test
// iterates this enum to prove it. A mode only the UI can reach is a mode no
// headless assertion covers.
enum class DebugView : uint32_t {
  Lit = 0,
  TriangleId,
  Barycentric,
  Depth,
  Albedo,
  Normal,
  Roughness,
  Metallic,
  Displacement,
  Ao,
  kCount,
};

// The CLI name and the UI label for each view, in enum order. One table, so the
// two surfaces cannot drift.
struct DebugViewInfo {
  std::string_view cli;    // --debug-view <this>
  std::string_view label;  // what the Graphics debug window shows
};
const std::array<DebugViewInfo, size_t(DebugView::kCount)>& DebugViews();

// Returns kCount if `name` matches nothing.
DebugView DebugViewFromName(std::string_view name);

// Mirrors ResolveFrame in resolve.slang. All-float4 members: MSL pads a float3
// to 16 bytes, so a struct that looks packed here arrives strided on the GPU.
struct ResolveFrameUniform {
  glm::mat4 view_proj{1.0f};
  glm::vec4 camera_pos{0.0f};
  glm::vec4 sun_direction{0.0f, 1.0f, 0.0f, 0.0f};
  glm::vec4 sun_color{1.0f};
  glm::vec4 params{0.0f};  // x = debug view, y = near, z = far
  glm::vec4 ambient_sh[9]{};
};
static_assert(sizeof(ResolveFrameUniform) == 64 + 16 * 4 + 16 * 9);

// What the Scene lighting window drives. Angles rather than a vector, because
// that is what the window exposes and a vector would have to be re-derived from
// them anyway.
struct SunSettings {
  float azimuth_deg = 135.0f;
  float elevation_deg = 45.0f;
  glm::vec3 color{1.0f, 0.96f, 0.9f};
  float intensity = 3.0f;
};

// The direction to point AT the sun, from its angles. Elevation 90 is straight
// up; azimuth rotates around +y from -z. Pure and exposed so the mapping is
// unit-testable without a device -- it is the only logic behind the window.
glm::vec3 SunDirection(const SunSettings& sun);

class ResolvePass {
 public:
  // `target_format` must match the attachment this renders into -- the SCENE
  // target, which is 8-bit encoded regardless of what is being presented.
  static std::unique_ptr<ResolvePass> Create(rhi::IRhiDevice& device,
                                             slang::SlangCompiler& compiler,
                                             const MaterialPack& pack,
                                             rhi::Format target_format);

  // Recycles this frame's allocator slot. Call once per frame, after
  // IRhiDevice::BeginFrame -- a SKIPPED frame still consumes its slot.
  void BeginFrame(uint64_t frame_index);

  void SetCamera(const glm::mat4& view, const glm::mat4& proj,
                 glm::vec3 camera_world_pos, float near_m, float far_m);
  void SetSun(const SunSettings& sun);
  void SetAmbient(const glm::vec4 sh[9]);
  void SetView(DebugView view) { view_ = view; }
  DebugView View() const { return view_; }

  // WHERE this frame's uniform landed. The hazard it guards is invisible in a
  // rendered image until frames overlap, so a test asserts distinct offsets.
  uint32_t LastFrameOffset() const { return frame_offset_; }
  rhi::IBuffer* LastFrameBuffer() const { return frame_buffer_; }

  // Adds the resolve, reading `visbuffer` and writing `target`. The visibility
  // buffer's TEXTURE is passed too, because the binding table names it and a
  // table is immutable -- a resize replaces the texture and the table has to
  // follow.
  bool AddToGraph(graph::RenderGraph& graph, graph::ResourceHandle visbuffer,
                  rhi::ITexture* visbuffer_texture,
                  graph::ResourceHandle vertices, graph::ResourceHandle indices,
                  graph::ResourceHandle draws, graph::ResourceHandle target,
                  rhi::IBuffer* vertex_buffer, rhi::IBuffer* index_buffer,
                  rhi::IBuffer* draw_buffer);

 private:
  bool BuildTable(rhi::ITexture* visbuffer, rhi::IBuffer* vertices,
                  rhi::IBuffer* indices, rhi::IBuffer* draws,
                  rhi::IBuffer* frame_buffer);

  rhi::IRhiDevice* device_ = nullptr;
  const MaterialPack* pack_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  // A RING, for the same reason VisbufferPass has one: the shell keeps up to
  // three frames in flight and a plain memcpy into one buffer rewrites bytes an
  // older frame is still reading. Here that desynchronises the resolve from the
  // raster that produced the visibility buffer it is reading.
  std::unique_ptr<rhi::FrameAllocator> alloc_;
  rhi::BindingTablePtr table_;
  rhi::ITexture* table_visbuffer_ = nullptr;
  rhi::IBuffer* table_frame_buffer_ = nullptr;
  rhi::IBuffer* frame_buffer_ = nullptr;
  uint32_t frame_offset_ = 0;

  ResolveFrameUniform frame_{};
  DebugView view_ = DebugView::Lit;
};

}  // namespace badlands::object_viewer
