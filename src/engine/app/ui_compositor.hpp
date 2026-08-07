#pragma once

// The debug-UI overlay and the pass that lays it over the surface.
//
// EXTRACTED FROM RhiApp SO IT CAN BE TESTED. It was inline in the run loop,
// which meant the only way to check the composite was to open a window and look
// at a translucent panel -- and src/engine/CLAUDE.md records that this exact
// arrangement has already been got wrong once, in a way that is invisible on an
// SDR display and draws a dark ring around every window on an HDR one.
//
// It owns the overlay because ImGui blends in ENCODED space and the surface may
// not be encoded. See ui_composite.slang for the two forms and why there are
// two.

#include <cstdint>
#include <memory>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::rhi_app {

class UiCompositor {
 public:
  // The overlay ImGui draws into: 8-bit and ENCODED, which is the space UI is
  // authored for.
  static constexpr rhi::Format kOverlayFormat = rhi::Format::RGBA8Unorm;

  // `surface_format` and `surface_color_space` must be the swapchain's actual
  // ones, read back rather than assumed -- the composite form depends on
  // whether the surface can hold values above 1.
  static std::unique_ptr<UiCompositor> Create(
      rhi::IRhiDevice& device, slang::SlangCompiler& compiler,
      rhi::Format surface_format, rhi::ColorSpace surface_color_space,
      uint32_t width, uint32_t height);

  // Returns false after logging; the caller must not treat that as "keep the
  // old ones", because they are already gone.
  bool Resize(uint32_t width, uint32_t height);

  rhi::ITexture* Overlay() const { return overlay_.get(); }

  // Clears the overlay, then composites it onto `surface`. The caller draws
  // ImGui into OverlayHandle() between the two, which is why this is not one
  // call: the clear must precede the UI and the composite must follow it.
  graph::ResourceHandle BeginOverlay(graph::RenderGraph& graph);
  bool Composite(graph::RenderGraph& graph, graph::ResourceHandle overlay,
                 graph::ResourceHandle surface);

 private:
  UiCompositor() = default;

  rhi::IRhiDevice* device_ = nullptr;
  rhi::ShaderModulePtr vs_, fs_;
  rhi::RenderPipelinePtr pipeline_;
  rhi::BufferPtr params_;
  rhi::TexturePtr overlay_;
  rhi::BindingTablePtr table_;
};

}  // namespace badlands::rhi_app
