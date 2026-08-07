#pragma once

// The output pass: the scene target, converted once, onto the surface.
//
// WHY A SEPARATE TARGET AND PASS AT ALL, when the graph could just render into
// the backbuffer -- which is what it did before this existed:
//
//   Alpha blending happens in the SURFACE's space. Draw ImGui straight into an
//   extended-linear EDR surface and its panels blend in LINEAR space; a
//   50%-alpha white over black lands at encoded 0.735 instead of 0.5, so every
//   translucent panel washes out and every antialiased glyph edge shifts. UI is
//   authored for encoded-space blending and there is no flag that fixes that
//   after the fact.
//
// So the UI draws into its OWN overlay -- RGBA8Unorm, encoded, premultiplied --
// while the scene target is RGBA16Float and LINEAR, and this pass tonemaps the
// scene, composites the overlay over it, and converts once. The engine reached
// the same arrangement independently: shaders/common/ui_composite.wesl
// composites in encoded space and shaders/passes/tonemapping.wesl converts
// primaries only afterwards.
//
// The composite runs only where the overlay actually covers, and in LINEAR
// space on an extended-range surface -- encoding clamps, so an encoded
// composite destroyed the scene's headroom at any non-zero alpha.

#include <memory>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::object_viewer {

// Mirrors OutputParams in shaders/slang/object_viewer/output.slang.
//   [0] output mode (IS rhi::ColorSpace)
//   [1] 1 to tonemap the scene, 0 to pass it through
// A float4 because a constant buffer of one or two scalars is an object every
// backend pads differently, and that is not worth reasoning about.
struct OutputParamsUniform {
  float mode[4] = {0, 0, 0, 0};
};
static_assert(sizeof(OutputParamsUniform) == 16);

class OutputPass {
 public:
  // `surface_format` must match the attachment this writes into -- the
  // SWAPCHAIN's format, which is not necessarily the one that was requested
  // (tagging can fail and drop it). Take it from AppShell::SurfaceFormat().
  static std::unique_ptr<OutputPass> Create(rhi::IRhiDevice& device,
                                            slang::SlangCompiler& compiler,
                                            rhi::Format surface_format);

  // Adds the conversion from `scene` into `surface`. `mode` is the colour space
  // being presented in; the shader constant IS the enum (see the static_asserts
  // beside rhi::ColorSpace).
  //
  // Returns false, after logging, if the binding table could not be built for
  // this scene texture -- which a caller must not treat as "nothing to convert".
  // `scene` is RGBA16Float LINEAR scene-referred; `ui` is RGBA8Unorm ENCODED
  // and PREMULTIPLIED. `tonemap` applies the tone curve to the scene -- true for
  // a lit image, FALSE for a debug view, whose value is already display-referred
  // and would be silently re-graded by it.
  // Recycles this frame's allocator slot. Call once per frame, after
  // IRhiDevice::BeginFrame -- a SKIPPED frame still consumes its slot.
  void BeginFrame(uint64_t frame_index);

  bool AddToGraph(graph::RenderGraph& graph, graph::ResourceHandle scene,
                  rhi::ITexture* scene_texture, graph::ResourceHandle ui,
                  rhi::ITexture* ui_texture, graph::ResourceHandle surface,
                  rhi::ColorSpace mode, bool tonemap);

  // Where this frame's params landed. Same test hook as the other two passes.
  uint32_t LastFrameOffset() const { return params_offset_; }

 private:
  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  rhi::SamplerPtr sampler_;
  // A RING, like the visbuffer and resolve uniforms.
  //
  // This was a plain buffer written in place, under a comment claiming that
  // writing only on CHANGE made it safe. It does not: it makes the race rare.
  // Flipping the Graphics debug radio group changes `tonemap` mid-frame, and
  // the memcpy then lands in bytes that two already-submitted frames are still
  // reading -- so those frames present a lit image un-tonemapped, or a debug
  // view Reinhard-compressed, for a frame or two.
  std::unique_ptr<rhi::FrameAllocator> alloc_;
  rhi::IBuffer* params_buffer_ = nullptr;
  uint32_t params_offset_ = 0;

  // Rebuilt when the scene texture changes, which it does on every resize.
  // Cached rather than rebuilt per frame because a binding table is immutable
  // and allocating one per frame is exactly the record-path cost rule 11 bans.
  rhi::BindingTablePtr table_;
  rhi::ITexture* table_scene_ = nullptr;
  rhi::ITexture* table_ui_ = nullptr;
  rhi::IBuffer* table_params_ = nullptr;
};

}  // namespace badlands::object_viewer
