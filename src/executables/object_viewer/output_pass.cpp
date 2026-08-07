#include "executables/object_viewer/output_pass.hpp"

#include <cstring>

#include <spdlog/spdlog.h>

namespace badlands::object_viewer {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns. Named here so a reordering in output.slang
// is a one-line fix rather than a hunt.
constexpr uint32_t kParamsSlot = 0;
constexpr uint32_t kSceneSlot = 1;
constexpr uint32_t kUiSlot = 2;
constexpr uint32_t kSamplerSlot = 3;

}  // namespace

std::unique_ptr<OutputPass> OutputPass::Create(IRhiDevice& device,
                                               slang::SlangCompiler& compiler,
                                               Format surface_format) {
  auto pass = std::unique_ptr<OutputPass>(new OutputPass());
  pass->device_ = &device;

  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled =
        compiler.Get({.module = "output", .entry = entry},
                     slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("output::") + entry);
  };
  pass->vs_ = load("vs_fullscreen");
  pass->fs_ = load("fs_output");
  if (!pass->vs_ || !pass->fs_) return nullptr;

  pass->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = pass->vs_.get(),
       .vertex_entry = "vs_fullscreen",
       .fragment_shader = pass->fs_.get(),
       .fragment_entry = "fs_output",
       .color_formats = {surface_format},
       // No blend state: this pass REPLACES the surface rather than compositing
       // onto it, which is the whole point -- everything that needed blending
       // already blended, in the scene target's encoded space.
       .cull_mode = CullMode::None,
       .label = "output"});
  if (!pass->pipeline_) return nullptr;

  // Nearest, and never sampled anyway (the shader uses Load). Declared because
  // the shader declares a SamplerState and a table entry it does not fill is a
  // refusal, not a default.
  pass->sampler_ = device.CreateSampler({.mag_filter = FilterMode::Nearest,
                                         .min_filter = FilterMode::Nearest,
                                         .mip_filter = FilterMode::Nearest,
                                         .address_u = AddressMode::ClampToEdge,
                                         .address_v = AddressMode::ClampToEdge,
                                         .label = "output"});
  pass->params_ = device.CreateBuffer(
      {.size = sizeof(OutputParamsUniform),
       .usage = BufferUsage::Uniform | BufferUsage::CopyDst,
       .label = "output_params"});
  if (!pass->sampler_ || !pass->params_) return nullptr;
  return pass;
}

bool OutputPass::AddToGraph(graph::RenderGraph& graph,
                            graph::ResourceHandle scene,
                            ITexture* scene_texture, graph::ResourceHandle ui,
                            ITexture* ui_texture,
                            graph::ResourceHandle surface, ColorSpace mode,
                            bool tonemap) {
  if (!scene_texture || !ui_texture) {
    spdlog::error("object_viewer: output pass given a null {} texture",
                  scene_texture ? "ui" : "scene");
    return false;
  }

  // Written only when it CHANGES. A per-frame write of an unchanged 16 bytes is
  // a copy the frame does not need, and worse, it is a write to a buffer a
  // frame still in flight may be reading.
  if (mode != written_mode_ || tonemap != written_tonemap_ || !params_written_) {
    OutputParamsUniform p;
    p.mode[0] = float(uint8_t(mode));
    p.mode[1] = tonemap ? 1.0f : 0.0f;
    params_->Write(0, {reinterpret_cast<const uint8_t*>(&p), sizeof(p)});
    written_mode_ = mode;
    written_tonemap_ = tonemap;
    params_written_ = true;
  }

  // The scene texture is recreated on every resize, and a binding table is
  // immutable -- so a table built against the old one would sample a destroyed
  // texture. Keyed on the pointer rather than rebuilt per frame, because
  // rebuilding per frame is the record-path allocation rule 11 bans.
  if (table_scene_ != scene_texture || table_ui_ != ui_texture || !table_) {
    auto table = device_->CreateBindingTable(
        {.render_pipeline = pipeline_.get(),
         .entries = {{.slot = kParamsSlot,
                      .kind = BindingKind::UniformBuffer,
                      .buffer = params_.get()},
                     {.slot = kSceneSlot,
                      .kind = BindingKind::SampledTexture,
                      .texture_view = scene_texture->GetDefaultView()},
                     {.slot = kUiSlot,
                      .kind = BindingKind::SampledTexture,
                      .texture_view = ui_texture->GetDefaultView()},
                     {.slot = kSamplerSlot,
                      .kind = BindingKind::Sampler,
                      .sampler = sampler_.get()}},
         .label = "output"});
    if (!table) return false;  // CreateBindingTable logged why
    table_ = std::move(table);
    table_scene_ = scene_texture;
    table_ui_ = ui_texture;
  }

  // Declared to the graph, not just bound. Every resource a table names needs a
  // transition derived for it, and the validation layer is what says so -- the
  // uniform buffer is written by the CPU outside any pass, so Undefined is its
  // honest entry state.
  auto params_h =
      graph.ImportBuffer(params_.get(), ResourceState::Undefined, "output_params");
  if (!params_h.IsValid()) return false;

  graph.AddRasterPass("output")
      .ColorTarget(surface, LoadOp::Clear, StoreOp::Store)
      .Reads(scene)
      .Reads(ui)
      .Reads(params_h)
      .Execute([this](const graph::RasterContext& ctx) {
        ctx.pass->SetPipeline(pipeline_.get());
        ctx.pass->SetBindingTable(0, table_.get(), {});
        ctx.pass->Draw(3);  // one fullscreen triangle
      });
  return true;
}

}  // namespace badlands::object_viewer
