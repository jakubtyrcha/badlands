#include "engine/app/ui_compositor.hpp"

#include <spdlog/spdlog.h>

namespace badlands::rhi_app {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns.
constexpr uint32_t kParamsSlot = 0;
constexpr uint32_t kOverlaySlot = 1;

struct CompositeParams {
  float mode[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

}  // namespace

std::unique_ptr<UiCompositor> UiCompositor::Create(
    IRhiDevice& device, slang::SlangCompiler& compiler, Format surface_format,
    ColorSpace surface_color_space, uint32_t width, uint32_t height) {
  auto out = std::unique_ptr<UiCompositor>(new UiCompositor());
  out->device_ = &device;

  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = "ui_composite", .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("ui_composite::") + entry);
  };
  out->vs_ = load("vs_ui_composite");
  out->fs_ = load("fs_ui_composite");
  if (!out->vs_ || !out->fs_) return nullptr;

  out->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = out->vs_.get(),
       .vertex_entry = "vs_ui_composite",
       .fragment_shader = out->fs_.get(),
       .fragment_entry = "fs_ui_composite",
       .color_formats = {surface_format},
       // PREMULTIPLIED. The overlay is a LAYER of accumulated translucent draws
       // and only this form composes associatively -- with straight alpha two
       // overlapping half-alpha draws double-count their colour against the
       // layer's own accumulated alpha.
       .blend_states = {PremultipliedAlphaBlend()},
       .cull_mode = CullMode::None,
       .label = "ui_composite"});
  if (!out->pipeline_) return nullptr;

  out->params_ = device.CreateBuffer(
      {.size = sizeof(CompositeParams),
       .usage = BufferUsage::Uniform | BufferUsage::CopyDst,
       .label = "ui_composite_params"});
  if (!out->params_) return nullptr;
  CompositeParams cp;
  // The enum IS the shader constant -- see the static_asserts beside
  // rhi::ColorSpace.
  cp.mode[0] = float(uint8_t(surface_color_space));
  out->params_->Write(0, {reinterpret_cast<const uint8_t*>(&cp), sizeof(cp)});

  if (!out->Resize(width, height)) return nullptr;
  return out;
}

bool UiCompositor::Resize(uint32_t width, uint32_t height) {
  overlay_ = device_->CreateTexture({.width = width,
                                     .height = height,
                                     .format = kOverlayFormat,
                                     .usage = TextureUsage::RenderTarget |
                                              TextureUsage::Sampled,
                                     .label = "ui_overlay"});
  if (!overlay_) return false;
  // The table NAMES the overlay and a table is immutable, so a resize that
  // rebuilt one without the other would sample a destroyed texture.
  table_ = device_->CreateBindingTable(
      {.render_pipeline = pipeline_.get(),
       .entries = {{.slot = kParamsSlot,
                    .kind = BindingKind::UniformBuffer,
                    .buffer = params_.get()},
                   {.slot = kOverlaySlot,
                    .kind = BindingKind::SampledTexture,
                    .texture_view = overlay_->GetDefaultView()}},
       .label = "ui_composite"});
  return table_ != nullptr;
}

graph::ResourceHandle UiCompositor::BeginOverlay(graph::RenderGraph& graph) {
  auto h = graph.ImportTexture(overlay_.get(), ResourceState::Undefined,
                               "ui_overlay");
  if (!h.IsValid()) return h;
  // CLEARED every frame: the overlay is rebuilt from scratch, and loading it
  // would accumulate last frame's panels.
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  graph.AddRasterPass("ui_clear")
      .ColorTarget(h, LoadOp::Clear, StoreOp::Store, clear)
      .Execute([](const graph::RasterContext&) {});
  return h;
}

bool UiCompositor::Composite(graph::RenderGraph& graph,
                             graph::ResourceHandle overlay,
                             graph::ResourceHandle surface) {
  auto params_h = graph.ImportBuffer(params_.get(), ResourceState::Undefined,
                                     "ui_composite_params");
  if (!params_h.IsValid() || !overlay.IsValid() || !surface.IsValid()) {
    spdlog::error("rhi_app: the UI composite was given an invalid handle");
    return false;
  }
  graph.AddRasterPass("ui_composite")
      .ColorTarget(surface, LoadOp::Load, StoreOp::Store)
      .Reads(overlay)
      .Reads(params_h)
      .Execute([this](const graph::RasterContext& c) {
        c.pass->SetPipeline(pipeline_.get());
        c.pass->SetBindingTable(0, table_.get());
        c.pass->Draw(3);  // one fullscreen triangle
      });
  return true;
}

}  // namespace badlands::rhi_app
