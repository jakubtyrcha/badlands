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
// So every pass draws into an offscreen target in ENCODED sRGB space, exactly
// as it did when the surface was 8-bit, and this pass performs the one
// conversion. The engine reached the same conclusion independently:
// shaders/common/ui_composite.wesl composites in encoded space and
// shaders/passes/tonemapping.wesl converts primaries only afterwards.
//
// STAGE 4D changes the target's format to RGBA16Float and gives this pass the
// tonemap. The conversion below does not change: a tonemap's job ends exactly
// where this one begins.

#include <memory>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
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
  bool AddToGraph(graph::RenderGraph& graph, graph::ResourceHandle scene,
                  rhi::ITexture* scene_texture, graph::ResourceHandle ui,
                  rhi::ITexture* ui_texture, graph::ResourceHandle surface,
                  rhi::ColorSpace mode, bool tonemap);

 private:
  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  rhi::SamplerPtr sampler_;
  rhi::BufferPtr params_;

  // Rebuilt when the scene texture changes, which it does on every resize.
  // Cached rather than rebuilt per frame because a binding table is immutable
  // and allocating one per frame is exactly the record-path cost rule 11 bans.
  rhi::BindingTablePtr table_;
  rhi::ITexture* table_scene_ = nullptr;
  rhi::ITexture* table_ui_ = nullptr;
  rhi::ColorSpace written_mode_ = rhi::ColorSpace::Srgb;
  bool written_tonemap_ = false;
  bool params_written_ = false;
};

}  // namespace badlands::object_viewer
