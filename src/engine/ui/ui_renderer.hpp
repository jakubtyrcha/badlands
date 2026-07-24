#pragma once

// Screen-space GAME UI renderer -- game-agnostic.
//
// This is the *game* UI surface, deliberately separate from the Dear ImGui
// *debug* UI (see CLAUDE.md). It knows nothing about heroes, gold or buildings:
// it consumes a flat list of UiQuads (physical pixels + atlas UVs + colour) and
// draws them in one alpha-blended pass. Producing those quads is the game
// layer's job (src/game/ui), which gets them from the `ui` Rust crate.
//
// Ownership: a view creates one of these in Initialize(RenderContext) alongside
// its material factories, and the APP runs the pass (AppView::GetUiRenderer) so
// no render-pass encoder ever leaks into view code. Views that want no game UI
// simply never create one.
//
// WHY ITS OWN UNIFORM BUFFER: this pass runs after SceneRenderer::Render() has
// returned, and that function's FrameContext is a stack local whose End()
// releases the frame UBO (context/frame_context.cpp:217). The engine's frame
// uniform buffer therefore does not exist by the time we draw, so shaders/ui/
// ui.wesl declares its own small UiFrame block and we own the buffer for it.
// That also makes this pass independent of whether the scene renderer ran.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "badlands_ui.h"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

class UiRenderer {
 public:
  UiRenderer() = default;
  ~UiRenderer();

  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;

  // Bakes the glyph atlas from `ttf_path` at `px_size` LOGICAL pixels (scaled
  // internally by `scale_factor`), uploads it, and builds the pipeline for
  // `surface_format`. Returns false (after logging) if the font is missing or
  // the shader fails.
  bool Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator& pipeline_gen,
                  wgpu::TextureFormat surface_format, const char* ttf_path,
                  float px_size, float scale_factor);

  bool ready() const { return ui_ctx_ != nullptr && !variants_.empty(); }

  // The Rust UiContext, so the game layer can call ui_build against the same
  // baked font this renderer draws with. Null until Initialize succeeds.
  UiContext* context() const { return ui_ctx_; }
  const UiFontInfo& font_info() const { return font_info_; }

  // Replace this frame's draw list. Expands quads to triangles and uploads.
  // An empty list is valid and makes Draw a no-op.
  void SetQuads(const UiQuad* quads, uint32_t count);

  // Point the renderer at a target: ensures a pipeline for `target_format` and
  // writes the screen size (PHYSICAL pixels) into the uniform buffer. Call
  // BEFORE opening the render pass -- it may create GPU resources and write a
  // buffer. Both are no-ops when nothing changed.
  //
  // Target-format-aware because the same UI is drawn to two different targets:
  // the BGRA8Unorm window surface and the RGBA8Unorm offscreen screenshot
  // texture. A pipeline is baked against its colour format, so each needs its
  // own; `variants_` caches them.
  //
  // output_is_p3: true when the target surface's CAMetalLayer is tagged
  // Display-P3 (GpuContext::IsP3()) -- the shader then converts its
  // sRGB-authored colors to P3 primaries. A per-target property, not a
  // per-format one: the window surface is P3-tagged, the offscreen
  // screenshot texture is not (captures stay sRGB-referred).
  void Prepare(uint32_t width_px, uint32_t height_px,
               wgpu::TextureFormat target_format, bool output_is_p3 = false);

  // Records the UI draw into an ALREADY-OPEN render pass on the target last
  // passed to Prepare (the app shares ImGui's LoadOp::Load pass, so game UI
  // lands under debug UI).
  void Draw(wgpu::RenderPassEncoder& pass);

 private:
  struct Variant {
    std::shared_ptr<const CompiledPipeline> pipeline;
    wgpu::BindGroup bind_group;
    uint32_t output_is_linear = 0;
  };

  void EnsureVertexCapacity(size_t bytes);
  // Builds (or returns the cached) pipeline + bind group for a colour format.
  Variant* EnsureVariant(wgpu::TextureFormat target_format);

  wgpu::Device device_;
  wgpu::Queue queue_;
  UiContext* ui_ctx_ = nullptr;
  UiFontInfo font_info_{};

  wgpu::TextureView atlas_view_;
  wgpu::Sampler sampler_;
  wgpu::Buffer frame_ubo_;
  wgpu::Buffer vertex_buffer_;
  size_t vertex_capacity_bytes_ = 0;
  uint32_t vertex_count_ = 0;

  GpuPipelineGenerator* pipeline_gen_ = nullptr;
  std::vector<std::pair<wgpu::TextureFormat, Variant>> variants_;
  Variant* active_ = nullptr;

  // CPU-side scratch, reused across frames to keep the per-frame path
  // allocation-free once it has reached its high-water mark.
  std::vector<float> vertices_;

  uint32_t viewport_w_ = 0;
  uint32_t viewport_h_ = 0;
  uint32_t uploaded_is_linear_ = 0xffffffffu;  // force the first write
  uint32_t uploaded_is_p3_ = 0xffffffffu;      // force the first write
};

}  // namespace badlands
