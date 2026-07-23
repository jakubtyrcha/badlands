#include "game/visual/cone_overlay_pass.hpp"

#include <array>

#include "engine/core/camera.hpp"  // UniformData (frame UBO size)
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/scene_renderer.hpp"  // SceneRenderer::kDepthFormat
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/vertex_layout.hpp"

namespace badlands {

bool ConeOverlayPass::Initialize(wgpu::Device device, wgpu::Queue queue,
                                 GpuPipelineGenerator* pipeline_gen) {
  device_ = device;
  queue_ = queue;
  pipeline_gen_ = pipeline_gen;
  return device_ && queue_ && pipeline_gen_ != nullptr;
}

void ConeOverlayPass::Execute(const PostSceneContext& ctx) {
  if (!enabled_ || pending_.empty() || pipeline_gen_ == nullptr) {
    return;
  }
  const uint32_t vertex_count = static_cast<uint32_t>(pending_.size() / 7);
  if (vertex_count < 3) {
    return;
  }

  // (Re)upload the triangle soup.
  const uint64_t bytes = pending_.size() * sizeof(float);
  if (!vbuf_ || bytes > vbuf_capacity_) {
    wgpu::BufferDescriptor bd;
    bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    bd.size = bytes;
    vbuf_ = device_.CreateBuffer(&bd);
    vbuf_capacity_ = bytes;
  }
  queue_.WriteBuffer(vbuf_, 0, pending_.data(), bytes);

  // Flat colored triangles, alpha-blended over the lit scene, depth-tested
  // against the G-buffer (reversed-Z) but never writing depth -- so buildings
  // occlude the cones and the cones do not occlude each other oddly.
  RenderPipelineDeclaration decl;
  decl.shader_path = "game/overlay";
  decl.vertex_layout = VertexLayout::kPolygon;  // pos(vec3) + color(vec4)
  decl.topology = wgpu::PrimitiveTopology::TriangleList;
  decl.cull_mode = wgpu::CullMode::None;
  wgpu::BlendState blend;
  blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
  blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.color.operation = wgpu::BlendOperation::Add;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.alpha.operation = wgpu::BlendOperation::Add;
  decl.custom_blend = blend;
  decl.depth_write = false;
  decl.depth_compare = wgpu::CompareFunction::GreaterEqual;  // reversed-Z
  decl.depth_format = SceneRenderer::kDepthFormat;

  RenderTargetFormats formats = {ctx.color_format};
  auto compiled = pipeline_gen_->GetPipeline(decl, formats);
  if (!compiled) {
    return;
  }

  // overlay.wesl only reads the frame UBO (group 0, binding 0).
  std::array<wgpu::BindGroupEntry, 1> entries{};
  entries[0].binding = 0;
  entries[0].buffer = ctx.frame.GetFrameUniformBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(UniformData);
  wgpu::BindGroup bind_group =
      ctx.frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = ctx.color_target;
  color_attachment.loadOp = wgpu::LoadOp::Load;
  color_attachment.storeOp = wgpu::StoreOp::Store;

  // Read-only depth: test against it, never write (pipeline depth_write=false).
  wgpu::RenderPassDepthStencilAttachment depth_attachment;
  depth_attachment.view = ctx.depth;
  depth_attachment.depthReadOnly = true;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;
  desc.depthStencilAttachment = &depth_attachment;

  RenderPassContext pass = ctx.frame.BeginRenderPass(desc);
  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.SetVertexBuffer(0, vbuf_);
  pass.Draw(vertex_count);
  pass.End();
}

}  // namespace badlands
