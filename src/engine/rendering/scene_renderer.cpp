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
#include <cstdint>

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/passes/render_gbuffer_debug.hpp"
#include "engine/rendering/passes/render_skybox.hpp"
#include "engine/rendering/passes/render_textured_mesh.hpp"

namespace badlands {

void SceneRenderer::Initialize(wgpu::Device device, wgpu::Queue queue,
                               GpuPipelineGenerator* pipeline_gen,
                               wgpu::TextureFormat surface_format,
                               uint32_t width, uint32_t height,
                               bool has_r8unorm_storage) {
  device_ = device;
  queue_ = queue;
  pipeline_generator_ = pipeline_gen;
  surface_format_ = surface_format;
  accumulation_format_ = kAccumulationFormat;
  has_r8unorm_storage_ = has_r8unorm_storage;

  wgpu::Limits limits{};
  if (device_.GetLimits(&limits) == wgpu::Status::Success) {
    min_uniform_offset_alignment_ = limits.minUniformBufferOffsetAlignment;
  }

  CreateTargets(width, height);

  // === IBL setup ===
  // BRDF split-sum LUT — one-time (roughness-only integral, view-independent).
  brdf_lut_.Generate(device_, queue_, *pipeline_generator_);

  // 1x1 black fallback cube + filtering sampler. The deferred-lighting IBL
  // bindings (@9/@10) must always be valid, even with no skybox — and this
  // sampler also serves as the source-cube sampler for the prefilter pass
  // (linear/clamp is exactly what GGX pre-filtering wants).
  {
    wgpu::TextureDescriptor desc;
    desc.size = {1, 1, 6};
    desc.format = wgpu::TextureFormat::RGBA16Float;
    desc.usage =
        wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    desc.dimension = wgpu::TextureDimension::e2D;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    fallback_cube_texture_ = device_.CreateTexture(&desc);

    std::array<uint16_t, 6 * 4> zeros{};  // RGBA16Float black, all 6 faces
    wgpu::TexelCopyTextureInfo dst;
    dst.texture = fallback_cube_texture_;
    dst.mipLevel = 0;
    dst.origin = {0, 0, 0};
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = 4 * sizeof(uint16_t);
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1, 1, 6};
    queue_.WriteTexture(&dst, zeros.data(), zeros.size() * sizeof(uint16_t),
                        &layout, &extent);

    wgpu::TextureViewDescriptor vdesc;
    vdesc.format = wgpu::TextureFormat::RGBA16Float;
    vdesc.dimension = wgpu::TextureViewDimension::Cube;
    vdesc.baseMipLevel = 0;
    vdesc.mipLevelCount = 1;
    vdesc.baseArrayLayer = 0;
    vdesc.arrayLayerCount = 6;
    fallback_cube_view_ = fallback_cube_texture_.CreateView(&vdesc);

    wgpu::SamplerDescriptor sdesc;
    sdesc.addressModeU = wgpu::AddressMode::ClampToEdge;
    sdesc.addressModeV = wgpu::AddressMode::ClampToEdge;
    sdesc.addressModeW = wgpu::AddressMode::ClampToEdge;
    sdesc.minFilter = wgpu::FilterMode::Linear;
    sdesc.magFilter = wgpu::FilterMode::Linear;
    sdesc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    sdesc.maxAnisotropy = 1;
    fallback_cube_sampler_ = device_.CreateSampler(&sdesc);
  }
}

void SceneRenderer::UpdateIbl(const SceneContext& scene) {
  if (!scene.skybox_cubemap) return;  // no source env -> fallback stays bound

  const bool source_changed =
      scene.skybox_cubemap.Get() != ibl_source_view_.Get();
  if (has_prefiltered_ && !source_changed &&
      scene.skybox_generation == ibl_source_generation_) {
    return;  // up to date
  }

  if (prefiltered_.Generate(device_, queue_, *pipeline_generator_,
                            scene.skybox_cubemap, fallback_cube_sampler_)) {
    has_prefiltered_ = true;
    ibl_source_view_ = scene.skybox_cubemap;
    ibl_source_generation_ = scene.skybox_generation;
  }
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

  // G-buffer (3 MRT color targets + reversed-Z depth). Its depth target is
  // this renderer's depth buffer; the deferred lighting pass samples it.
  gbuffer_.Create(device_, width, height);
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

  // Refresh the prefiltered specular env if the scene's skybox changed. Does
  // its own encoder/submit (queue-ordered before this frame's work below).
  UpdateIbl(scene);

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

  // === Pass 1: G-buffer — clear MRT (normals/albedo/material) + depth,
  // draw textured meshes with their kGBuffer pipeline variant ===
  {
    std::array<wgpu::RenderPassColorAttachment, 3> color_attachments{};

    // Normals (RG16Float, octahedron-encoded world normal).
    color_attachments[0].view = gbuffer_.GetNormalsView();
    color_attachments[0].loadOp = wgpu::LoadOp::Clear;
    color_attachments[0].storeOp = wgpu::StoreOp::Store;
    color_attachments[0].clearValue = {0.0, 0.0, 0.0, 0.0};

    // Albedo (BGRA8Unorm, base color).
    color_attachments[1].view = gbuffer_.GetAlbedoView();
    color_attachments[1].loadOp = wgpu::LoadOp::Clear;
    color_attachments[1].storeOp = wgpu::StoreOp::Store;
    color_attachments[1].clearValue = {0.0, 0.0, 0.0, 0.0};

    // Material (RGBA8Unorm: R=roughness default 0.75, G=shadow 1.0 = lit,
    // B=AO default 1.0 = no occlusion).
    color_attachments[2].view = gbuffer_.GetMaterialView();
    color_attachments[2].loadOp = wgpu::LoadOp::Clear;
    color_attachments[2].storeOp = wgpu::StoreOp::Store;
    color_attachments[2].clearValue = {0.75, 1.0, 1.0, 0.0};

    wgpu::RenderPassDepthStencilAttachment depth_attachment;
    depth_attachment.view = gbuffer_.GetDepthView();
    depth_attachment.depthClearValue = 0.0f;  // reversed-Z: 0 = far
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = color_attachments.size();
    desc.colorAttachments = color_attachments.data();
    desc.depthStencilAttachment = &depth_attachment;

    RenderPassContext pass = frame.BeginRenderPass(desc);
    RenderTexturedMeshes(pass, frame, registry, camera_world_pos,
                         RenderPassType::kGBuffer, material_instance_cache_);
    pass.End();
  }

  // === Pass 2: Skybox background — fill the HDR target from the source
  // environment cube (fullscreen, standard cube sampling along the per-pixel
  // world ray). Runs BEFORE deferred lighting: it clears + fills the whole
  // HDR, then deferred lighting (Pass 3, LoadOp::Load) discards far/sky pixels
  // so the skybox shows through wherever there is no geometry.
  //
  // When the scene has no source cube (scene.skybox_cubemap null), this pass is
  // skipped and Pass 3 falls back to clearing scene.clear_color (B1/B2
  // behavior). ===
  const bool have_skybox = static_cast<bool>(scene.skybox_cubemap);
  if (have_skybox) {
    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = hdr_color_view_;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    // clearValue is irrelevant — the fullscreen skybox overwrites every pixel.
    color_attachment.clearValue = {0.0, 0.0, 0.0, 1.0};

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = nullptr;  // no depth: fills the whole target

    RenderPassContext pass = frame.BeginRenderPass(desc);
    RenderSkybox(pass, frame, *pipeline_generator_, accumulation_format_,
                 scene.skybox_cubemap, fallback_cube_sampler_);
    pass.End();
  }

  // === Pass 3: Deferred lighting — fullscreen pass reading the G-buffer,
  // lighting it (sun directional + SH ambient + IBL) into the HDR target.
  // With a skybox present it LOADs (keeps the skybox and discards far/sky
  // pixels); otherwise it CLEARs to scene.clear_color so sky/no-geometry
  // pixels keep the background color. ===
  {
    RenderPipelineDeclaration decl;
    decl.shader_path = "passes/deferred_lighting";
    // vertex_layout defaults to VertexLayout::kFullscreen (fullscreen
    // triangle from @builtin(vertex_index); no vertex buffer, no depth).

    RenderTargetFormats target_formats = {accumulation_format_};
    auto compiled = pipeline_generator_->GetPipeline(decl, target_formats);
    if (!compiled) {
      spdlog::error(
          "SceneRenderer::Render: failed to compile deferred_lighting "
          "pipeline");
    } else {
      // Group 0: frame UBO @0, gbuffer depth @1, normals @2, albedo @3,
      // material @4 (all textureLoad'd — no sampler), then IBL: prefiltered
      // env cube @9 + sampler @10, BRDF LUT @11 + sampler @12. Matches
      // shaders/passes/deferred_lighting.wesl's binding declarations. The IBL
      // cube is the prefiltered env when available, else a 1x1 black fallback
      // (so the specular term is zero) — bindings are always valid.
      const bool use_prefiltered = has_prefiltered_ && prefiltered_.IsValid();
      wgpu::TextureView ibl_cube_view =
          use_prefiltered ? prefiltered_.GetView() : fallback_cube_view_;
      wgpu::Sampler ibl_cube_sampler =
          use_prefiltered ? prefiltered_.GetSampler() : fallback_cube_sampler_;

      std::array<wgpu::BindGroupEntry, 9> entries{};
      entries[0].binding = 0;
      entries[0].buffer = frame.GetFrameUniformBuffer();
      entries[0].offset = 0;
      entries[0].size = sizeof(UniformData);
      entries[1].binding = 1;
      entries[1].textureView = gbuffer_.GetDepthView();
      entries[2].binding = 2;
      entries[2].textureView = gbuffer_.GetNormalsView();
      entries[3].binding = 3;
      entries[3].textureView = gbuffer_.GetAlbedoView();
      entries[4].binding = 4;
      entries[4].textureView = gbuffer_.GetMaterialView();
      entries[5].binding = 9;
      entries[5].textureView = ibl_cube_view;
      entries[6].binding = 10;
      entries[6].sampler = ibl_cube_sampler;
      entries[7].binding = 11;
      entries[7].textureView = brdf_lut_.GetView();
      entries[8].binding = 12;
      entries[8].sampler = brdf_lut_.GetSampler();

      wgpu::BindGroup bind_group =
          frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

      wgpu::RenderPassColorAttachment color_attachment;
      color_attachment.view = hdr_color_view_;
      color_attachment.loadOp =
          have_skybox ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
      color_attachment.storeOp = wgpu::StoreOp::Store;
      color_attachment.clearValue = {scene.clear_color.r, scene.clear_color.g,
                                     scene.clear_color.b, scene.clear_color.a};

      wgpu::RenderPassDescriptor desc;
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color_attachment;
      desc.depthStencilAttachment = nullptr;  // reads depth as a texture

      RenderPassContext pass = frame.BeginRenderPass(desc);
      pass.SetPipeline(compiled->pipeline);
      pass.SetBindGroup(0, bind_group);
      pass.Draw(3);
      pass.End();
    }
  }

  // === Pass 4: Final resolve: HDR -> surface. Normally the tonemap pass;
  // when a G-buffer debug mode is selected (SetDebugMode), gbuffer_debug.wesl
  // visualizes the selected channel instead (S2.B4). ===
  {
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

    if (debug_mode_ != GBufferDebugMode::None) {
      RenderGBufferDebug(pass, frame, *pipeline_generator_, surface_format_,
                         gbuffer_.GetDepthView(), gbuffer_.GetNormalsView(),
                         gbuffer_.GetAlbedoView(), gbuffer_.GetMaterialView(),
                         hdr_color_view_, debug_mode_);
    } else {
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

        pass.SetPipeline(compiled->pipeline);
        pass.SetBindGroup(0, bind_group);
        pass.Draw(3);
      }
    }

    pass.End();
  }

  wgpu::CommandBuffer commands = frame.End();
  queue_.Submit(1, &commands);
}

}  // namespace badlands
