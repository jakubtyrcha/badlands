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
  Uv,
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
  // x = prefiltered mip count, y = environment intensity, z = 1 when the IBL
  // textures hold a real environment.
  //
  // z EXISTS BECAUSE A BINDING CANNOT BE ABSENT. The table always names a cube,
  // so a 1x1 dummy and a real environment are indistinguishable to the shader;
  // without the flag the viewer would light everything with black and look
  // merely dark rather than broken.
  glm::vec4 ibl{5.0f, 1.0f, 0.0f, 0.0f};
  // The frustum basis the background ray is built from. Three vectors rather
  // than an inverse view-projection, because the resolve deliberately has no
  // matrix inverse and a background needs a direction, not a position.
  glm::vec4 ray_forward{0.0f, 0.0f, -1.0f, 0.0f};
  glm::vec4 ray_right{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec4 ray_up{0.0f, 1.0f, 0.0f, 0.0f};
  glm::vec4 ambient_sh[9]{};
};
static_assert(sizeof(ResolveFrameUniform) == 64 + 16 * 8 + 16 * 9);

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
  // target, which is RGBA16Float and LINEAR regardless of what is being
  // presented. The lit view therefore emits SCENE-REFERRED values that may
  // exceed 1; the tonemap lives in the output pass.
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

  // The IBL chain this frame samples. Passing null for either texture CLEARS
  // the flag rather than leaving a stale one set -- a resolve pointed at a
  // destroyed cube is worse than one with no environment at all.
  void SetEnvironment(rhi::ITextureView* prefiltered, uint32_t mip_count,
                      rhi::ITextureView* brdf_lut, float intensity);

  // Intensity alone, which is a multiply in the shader and names no resource --
  // so unlike SetEnvironment this must NOT drop the binding table. Rebuilding a
  // table every frame is the record-path allocation rule 11 bans.
  void SetEnvironmentIntensity(float intensity) { frame_.ibl.y = intensity; }

  // The camera basis the background ray uses. Separate from SetCamera because
  // the viewer's Camera carries the projection parameters and this pass does
  // not -- deriving it here would need a second copy of the fov and aspect.
  void SetViewRays(glm::vec3 forward, glm::vec3 right, glm::vec3 up);
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
  // `depth` is the visibility buffer's own depth, declared READ-ONLY: the
  // resolve and the background are two disjoint depth-tested draws over it
  // rather than one fullscreen pass that branches. See background.slang.
  bool AddToGraph(graph::RenderGraph& graph, graph::ResourceHandle visbuffer,
                  rhi::ITexture* visbuffer_texture,
                  graph::ResourceHandle depth,
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
  // The BACKGROUND half. Its own pipeline because the depth compare is pipeline
  // state, and its own table because a table resolves slots against one
  // pipeline's reflection.
  rhi::ShaderModulePtr bg_vs_, bg_fs_;
  rhi::RenderPipelinePtr bg_pipeline_;
  rhi::BindingTablePtr bg_table_;
  rhi::IBuffer* bg_table_frame_ = nullptr;
  // A RING, for the same reason VisbufferPass has one: the shell keeps up to
  // three frames in flight and a plain memcpy into one buffer rewrites bytes an
  // older frame is still reading. Here that desynchronises the resolve from the
  // raster that produced the visibility buffer it is reading.
  std::unique_ptr<rhi::FrameAllocator> alloc_;
  rhi::BindingTablePtr table_;
  rhi::SamplerPtr ibl_sampler_;
  // The 1x1 stand-ins bound when there is no environment. A table entry cannot
  // be empty, and binding a destroyed view is a validation error -- so "no IBL"
  // is a real (tiny) texture plus the flag above, not a missing binding.
  rhi::TexturePtr dummy_cube_, dummy_lut_;
  rhi::ITextureView* env_view_ = nullptr;
  rhi::ITextureView* lut_view_ = nullptr;
  rhi::ITexture* table_visbuffer_ = nullptr;
  rhi::IBuffer* table_frame_buffer_ = nullptr;
  rhi::IBuffer* frame_buffer_ = nullptr;
  uint32_t frame_offset_ = 0;

  ResolveFrameUniform frame_{};
  DebugView view_ = DebugView::Lit;
};

}  // namespace badlands::object_viewer
