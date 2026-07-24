#include "engine/rendering/color_grading.hpp"

#include <array>

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/gpu_timer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {
namespace {

// Must match shaders/passes/color_grading.wesl's ColorGradingParams
// (8 x f32 = 32 bytes; the trailing pads keep the uniform block 16-aligned).
struct ColorGradingParamsUniform {
  float black_crush_threshold;
  float black_crush_strength;
  float midtone_luminance_start;
  float midtone_luminance_end;
  float midtone_desat_strength;
  float saturation_preservation_mask;
  float pad0;
  float pad1;
};
static_assert(sizeof(ColorGradingParamsUniform) == 32,
              "params must match color_grading.wesl");

}  // namespace

void ColorGrading::Initialize(GpuPipelineGenerator* gen,
                              wgpu::TextureFormat hdr_format) {
  pipeline_generator_ = gen;
  hdr_format_ = hdr_format;
}

void ColorGrading::Render(FrameContext& frame, GpuTimer& gpu_timer,
                          wgpu::TextureView source, wgpu::TextureView target) {
  if (!pipeline_generator_ || !source || !target) return;

  RenderPipelineDeclaration decl;
  decl.shader_path = "passes/color_grading";
  // vertex_layout defaults to VertexLayout::kFullscreen (fullscreen triangle
  // from @builtin(vertex_index); no vertex buffer, no depth).

  RenderTargetFormats target_formats = {hdr_format_};
  auto compiled = pipeline_generator_->GetPipeline(decl, target_formats);
  if (!compiled) {
    if (!logged_compile_error_) {
      logged_compile_error_ = true;
      spdlog::error("ColorGrading::Render: failed to compile color_grading");
    }
    return;
  }

  // Transient per-frame params UBO (auto-freed at FrameContext::End()).
  ColorGradingParamsUniform params{};
  params.black_crush_threshold = config_.black_crush_threshold;
  params.black_crush_strength = config_.black_crush_strength;
  params.midtone_luminance_start = config_.midtone_luminance_start;
  params.midtone_luminance_end = config_.midtone_luminance_end;
  params.midtone_desat_strength = config_.midtone_desat_strength;
  params.saturation_preservation_mask = config_.saturation_preservation_mask;
  wgpu::Buffer params_buf =
      frame.CreateUniformBuffer(sizeof(params), &params);

  // Group 0: HDR snapshot @0 (textureLoad'd — no sampler), params UBO @1.
  std::array<wgpu::BindGroupEntry, 2> entries{};
  entries[0].binding = 0;
  entries[0].textureView = source;
  entries[1].binding = 1;
  entries[1].buffer = params_buf;
  entries[1].offset = 0;
  entries[1].size = sizeof(params);
  wgpu::BindGroup bind_group =
      frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = target;
  // LoadOp irrelevant — the fullscreen triangle overwrites every pixel — but
  // Load is cheaper than a pointless clear on tilers.
  color_attachment.loadOp = wgpu::LoadOp::Load;
  color_attachment.storeOp = wgpu::StoreOp::Store;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;
  desc.depthStencilAttachment = nullptr;

  desc.timestampWrites = gpu_timer.BeginPass("grade");
  RenderPassContext pass = frame.BeginRenderPass(desc);
  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.Draw(3);
  pass.End();
}

}  // namespace badlands
