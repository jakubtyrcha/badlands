// Trimmed port of sampo's src/rendering/scene_renderer.cpp, namespace sampo
// -> badlands. See the header's file-level comment for the itemized list of
// sampo passes/subsystems this drops.
//
// The tonemap resolve pass is hand-rolled directly against
// GpuPipelineGenerator/FrameContext/RenderPassContext instead of via
// sampo's `ProcessingGraph` + `FullscreenShaderNode`/`TonemappingNode`
// (`image_processing/**`, not ported — a general-purpose DAG compute/render
// graph compositor, overkill for this renderer's single fixed fullscreen
// pass). The bind-group layout it targets (group 0: binding 0 = frame UBO,
// binding 1 = HDR texture, `textureLoad`'d — no sampler) and the pipeline
// declaration (`shader_path = "passes/tonemapping"`, default
// VertexLayout::kFullscreen) are exactly what `TonemappingNode`/
// `FullscreenShaderNode::Execute` built, just assembled inline.
#include "engine/rendering/scene_renderer.hpp"

#include <array>

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/passes/render_textured_mesh.hpp"

namespace badlands {

void SceneRenderer::Initialize(wgpu::Device device, wgpu::Queue queue,
                               GpuPipelineGenerator* pipeline_gen,
                               wgpu::TextureFormat surface_format,
                               uint32_t width, uint32_t height) {
  device_ = device;
  queue_ = queue;
  pipeline_generator_ = pipeline_gen;
  surface_format_ = surface_format;
  accumulation_format_ = kAccumulationFormat;

  wgpu::Limits limits{};
  if (device_.GetLimits(&limits) == wgpu::Status::Success) {
    min_uniform_offset_alignment_ = limits.minUniformBufferOffsetAlignment;
  }

  CreateTargets(width, height);
}

void SceneRenderer::CreateTargets(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

  wgpu::TextureDescriptor hdr_desc;
  hdr_desc.size = {width, height, 1};
  hdr_desc.format = accumulation_format_;
  hdr_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  hdr_desc.mipLevelCount = 1;
  hdr_desc.sampleCount = 1;
  hdr_desc.dimension = wgpu::TextureDimension::e2D;
  hdr_color_texture_ = device_.CreateTexture(&hdr_desc);
  hdr_color_view_ = hdr_color_texture_.CreateView();

  wgpu::TextureDescriptor depth_desc;
  depth_desc.size = {width, height, 1};
  depth_desc.format = kDepthFormat;
  depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
  depth_desc.mipLevelCount = 1;
  depth_desc.sampleCount = 1;
  depth_desc.dimension = wgpu::TextureDimension::e2D;
  depth_texture_ = device_.CreateTexture(&depth_desc);
  depth_view_ = depth_texture_.CreateView();
}

void SceneRenderer::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 ||
      (width == width_ && height == height_)) {
    return;
  }
  CreateTargets(width, height);
}

void SceneRenderer::Render(const Camera& camera, entt::registry& registry,
                           const SceneContext& scene,
                           wgpu::TextureView surface_view) {
  if (!surface_view || width_ == 0 || height_ == 0 || !pipeline_generator_) {
    return;
  }

  // World camera-offsetting: the view matrix has the camera fixed at the
  // origin (pure rotation, no translation) for float precision — geometry is
  // transformed into WorldCameraOffsetedSpace (world_pos - camera_world_pos)
  // per-object instead (see render_textured_mesh.cpp). Deliberately NOT
  // `camera.GetView()` (Camera::GetView() bakes in a translate by
  // -position, which would double-subtract the camera offset already
  // applied to vertex positions).
  glm::vec3 camera_world_pos = camera.GetPosition();

  UniformData frame_uniforms{};
  frame_uniforms.view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  frame_uniforms.proj = camera.GetProj();
  // No motion vectors/TAA yet — previous-frame matrices equal current.
  frame_uniforms.view_prev = frame_uniforms.view;
  frame_uniforms.proj_prev = frame_uniforms.proj;
  frame_uniforms.light_view_proj = glm::mat4(1.0f);  // no shadow map
  frame_uniforms.camera_world_pos = glm::vec4(camera_world_pos, 0.0f);
  frame_uniforms.sunDir = glm::vec4(scene.sun_direction, 0.0f);
  frame_uniforms.sunColor = glm::vec4(scene.sun_color, 0.0f);
  for (int i = 0; i < 9; ++i) {
    frame_uniforms.ambient_sh[i] = glm::vec4(scene.ambient_sh[i], 0.0f);
  }
  frame_uniforms.sphere_offset = glm::vec4(0.0f);  // no terrain sphere
  frame_uniforms.jitter = glm::vec2(0.0f);
  frame_uniforms.jitter_prev = glm::vec2(0.0f);
  frame_uniforms.near_plane = camera.near_plane;
  frame_uniforms.far_plane = camera.far_plane;
  frame_uniforms.screen_size =
      glm::vec2(static_cast<float>(width_), static_cast<float>(height_));
  frame_uniforms.enable_gtao = 0;      // no GTAO pass
  frame_uniforms.tonemap_mode = 0;     // kClamp (TonemapMode not ported;
                                       // shaders/common/frame.wesl: 0 = kClamp)
  frame_uniforms.output_is_linear =
      (surface_format_ == wgpu::TextureFormat::RGBA16Float) ? 1 : 0;

  FrameContext frame;
  frame.Begin(device_, queue_, frame_uniforms, min_uniform_offset_alignment_);

  // === Forward-opaque pass: clear HDR + depth, draw textured meshes ===
  {
    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = hdr_color_view_;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {scene.clear_color.r, scene.clear_color.g,
                                   scene.clear_color.b, scene.clear_color.a};

    wgpu::RenderPassDepthStencilAttachment depth_attachment;
    depth_attachment.view = depth_view_;
    depth_attachment.depthClearValue = 0.0f;  // reversed-Z: 0 = far
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = &depth_attachment;

    RenderPassContext pass = frame.BeginRenderPass(desc);
    RenderTexturedMeshes(pass, frame, registry, camera_world_pos,
                         RenderPassType::kForward, material_instance_cache_);
    pass.End();
  }

  // === Tonemap resolve: HDR -> surface ===
  {
    RenderPipelineDeclaration decl;
    decl.shader_path = "passes/tonemapping";
    // vertex_layout defaults to VertexLayout::kFullscreen (no vertex
    // buffer — the shader generates a fullscreen triangle from
    // @builtin(vertex_index)); cull_mode/depth default to off, matching a
    // depth-less fullscreen blit.

    RenderTargetFormats target_formats = {surface_format_};
    auto compiled = pipeline_generator_->GetPipeline(decl, target_formats);
    if (!compiled) {
      spdlog::error(
          "SceneRenderer::Render: failed to compile tonemap pipeline");
    } else {
      std::array<wgpu::BindGroupEntry, 2> entries{};
      entries[0].binding = 0;
      entries[0].buffer = frame.GetFrameUniformBuffer();
      entries[0].offset = 0;
      entries[0].size = sizeof(UniformData);
      entries[1].binding = 1;
      entries[1].textureView = hdr_color_view_;

      wgpu::BindGroup bind_group =
          frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

      wgpu::RenderPassColorAttachment color_attachment;
      color_attachment.view = surface_view;
      color_attachment.loadOp = wgpu::LoadOp::Clear;
      color_attachment.storeOp = wgpu::StoreOp::Store;
      color_attachment.clearValue = {0.0, 0.0, 0.0, 1.0};

      wgpu::RenderPassDescriptor desc;
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color_attachment;
      desc.depthStencilAttachment = nullptr;

      RenderPassContext pass = frame.BeginRenderPass(desc);
      pass.SetPipeline(compiled->pipeline);
      pass.SetBindGroup(0, bind_group);
      pass.Draw(3);
      pass.End();
    }
  }

  wgpu::CommandBuffer commands = frame.End();
  queue_.Submit(1, &commands);
}

}  // namespace badlands
