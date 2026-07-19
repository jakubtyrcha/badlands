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

#include <algorithm>
#include <array>
#include <cstdint>

#include <iostream>

#include <spdlog/spdlog.h>

#include "core/profiler.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/passes/render_forward.hpp"
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

  // === Shadow-map comparison sampler (T3) ===
  // Hardware PCF (bilinear-filtered compare) for sampleShadowMapPCF's 3x3 tap
  // pattern. CompareFunction::LessEqual matches shadow_map_'s conventional-Z
  // depth (near->0, far->1, cleared to 1.0=far) -- a receiver is lit when
  // its (biased) depth is <= the stored occluder depth. Do NOT change to
  // GreaterEqual/Less -- see shadow_map.hpp's Z-convention note.
  {
    wgpu::SamplerDescriptor sampler_desc{};
    sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
    sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
    sampler_desc.addressModeW = wgpu::AddressMode::ClampToEdge;
    sampler_desc.magFilter = wgpu::FilterMode::Linear;
    sampler_desc.minFilter = wgpu::FilterMode::Linear;
    sampler_desc.maxAnisotropy = 1;
    sampler_desc.compare = wgpu::CompareFunction::LessEqual;
    shadow_comparison_sampler_ = device_.CreateSampler(&sampler_desc);
    if (!shadow_comparison_sampler_) {
      spdlog::error(
          "SceneRenderer::Initialize: failed to create shadow comparison "
          "sampler");
    }
  }

  // === Volumetric fog (Task: fog rendering) ===
  // Owns its media cascade 3D texture + fill/composite pipelines; the fog pass
  // runs between deferred lighting and tonemap, blending into the HDR target.
  volumetric_fog_.Initialize(device_, pipeline_generator_, accumulation_format_);

  // Map fog generator: produces the sim density field VolumetricFog reconstructs
  // the media from. Sources (emitters) are supplied by the game.
  fog_sim_.Initialize(device_, queue_);
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
  hdr_desc.usage = wgpu::TextureUsage::RenderAttachment |
                   wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::CopySrc;  // snapshot for water refraction
  hdr_desc.mipLevelCount = 1;
  hdr_desc.sampleCount = 1;
  hdr_desc.dimension = wgpu::TextureDimension::e2D;
  hdr_color_texture_ = device_.CreateTexture(&hdr_desc);
  hdr_color_view_ = hdr_color_texture_.CreateView();

  // Copy of the HDR scene colour, snapshotted before the forward-transparent
  // pass so the water material can sample the scene behind it (normal-driven
  // distortion / refraction) — a target can't be sampled while it's bound.
  wgpu::TextureDescriptor hdr_copy_desc = hdr_desc;
  hdr_copy_desc.usage =
      wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  hdr_color_copy_texture_ = device_.CreateTexture(&hdr_copy_desc);
  hdr_color_copy_view_ = hdr_color_copy_texture_.CreateView();

  if (!linear_clamp_sampler_) {
    wgpu::SamplerDescriptor s;
    s.magFilter = wgpu::FilterMode::Linear;
    s.minFilter = wgpu::FilterMode::Linear;
    s.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    s.addressModeU = wgpu::AddressMode::ClampToEdge;
    s.addressModeV = wgpu::AddressMode::ClampToEdge;
    s.addressModeW = wgpu::AddressMode::ClampToEdge;
    linear_clamp_sampler_ = device_.CreateSampler(&s);
  }

  // G-buffer (3 MRT color targets + reversed-Z depth). Its depth target is
  // this renderer's depth buffer; the deferred lighting pass samples it.
  gbuffer_.Create(device_, width, height);

  // === GTAO AO target (M6) ===
  // R8Unorm, G-buffer sized. Always TextureBinding (deferred pass reads it @7)
  // + RenderAttachment (the clear-to-1.0 pass below); StorageBinding is added
  // only when the device supports R8Unorm storage textures (the GTAO compute
  // pass writes it). Recreated here on every CreateTargets (Initialize/Resize).
  wgpu::TextureDescriptor ao_desc;
  ao_desc.size = {width, height, 1};
  ao_desc.format = wgpu::TextureFormat::R8Unorm;
  ao_desc.usage = wgpu::TextureUsage::TextureBinding |
                  wgpu::TextureUsage::RenderAttachment;
  if (has_r8unorm_storage_) {
    ao_desc.usage |= wgpu::TextureUsage::StorageBinding;
  }
  ao_desc.mipLevelCount = 1;
  ao_desc.sampleCount = 1;
  ao_desc.dimension = wgpu::TextureDimension::e2D;
  ao_texture_ = device_.CreateTexture(&ao_desc);
  ao_view_ = ao_texture_.CreateView();

  // Clear the AO target to 1.0 (fully unoccluded) once at creation so it is
  // valid even before/without a GTAO dispatch (e.g. no R8Unorm-storage device,
  // GTAO toggled off, or the first frame). A trivial RenderAttachment clear.
  {
    wgpu::RenderPassColorAttachment ao_clear;
    ao_clear.view = ao_view_;
    ao_clear.loadOp = wgpu::LoadOp::Clear;
    ao_clear.storeOp = wgpu::StoreOp::Store;
    ao_clear.clearValue = {1.0, 1.0, 1.0, 1.0};

    wgpu::RenderPassDescriptor clear_desc;
    clear_desc.colorAttachmentCount = 1;
    clear_desc.colorAttachments = &ao_clear;
    clear_desc.depthStencilAttachment = nullptr;

    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&clear_desc);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
  }

  // === Contact-shadow target (T2) ===
  // Window-sized R8Unorm. No create-time clear needed: T5's SSCS fullscreen
  // render pass (Pass 1.75, Render()) unconditionally clears this same view
  // (LoadOp::Clear) every frame, BEFORE the deferred pass (Pass 3) ever
  // reads it @8 -- so a stale/uninitialized first frame is not observable.
  // (Unlike ao_texture_ above, which the GTAO compute pass only
  // conditionally writes and can be read before that -- hence its
  // create-time clear stays.)
  wgpu::TextureDescriptor contact_desc;
  contact_desc.size = {width, height, 1};
  contact_desc.format = wgpu::TextureFormat::R8Unorm;
  contact_desc.usage = wgpu::TextureUsage::TextureBinding |
                       wgpu::TextureUsage::RenderAttachment;
  contact_desc.mipLevelCount = 1;
  contact_desc.sampleCount = 1;
  contact_desc.dimension = wgpu::TextureDimension::e2D;
  contact_shadow_texture_ = device_.CreateTexture(&contact_desc);
  contact_shadow_view_ = contact_shadow_texture_.CreateView();
}

void SceneRenderer::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 ||
      (width == width_ && height == height_)) {
    return;
  }
  CreateTargets(width, height);
}

void SceneRenderer::EnableGpuProfiling(wgpu::Instance instance,
                                       bool timestamp_supported) {
  gpu_timer_.Initialize(instance, device_, timestamp_supported);
}

void SceneRenderer::Render(const Camera& camera, entt::registry& registry,
                           const SceneContext& scene,
                           wgpu::TextureView surface_view) {
  if (!surface_view || width_ == 0 || height_ == 0 || !pipeline_generator_) {
    return;
  }
  PROFILE_SCOPE("SceneRenderer::Render");
  gpu_timer_.BeginFrame();

  // Refresh the prefiltered specular env if the scene's skybox changed. Does
  // its own encoder/submit (queue-ordered before this frame's work below).
  // (The GGX prefilter fires here whenever the daylight re-bake bumped the
  // skybox generation -- watch this marker for the IBL cost.)
  {
    PROFILE_SCOPE("UpdateIbl");
    UpdateIbl(scene);
  }

  // World camera-offsetting: the view matrix has the camera fixed at the
  // origin (pure rotation, no translation) for float precision — geometry is
  // transformed into WorldCameraOffsetedSpace (world_pos - camera_world_pos)
  // per-object instead (see render_textured_mesh.cpp). Deliberately NOT
  // `camera.GetView()` (Camera::GetView() bakes in a translate by
  // -position, which would double-subtract the camera offset already
  // applied to vertex positions).
  glm::vec3 camera_world_pos = camera.GetPosition();

  // GTAO runs only when enabled AND the device supports R8Unorm storage
  // textures. `enable_gtao` (set below) tells the deferred pass whether to read
  // the AO texture (else it uses 1.0 → final AO = baked AO).
  const bool run_gtao = has_r8unorm_storage_ && gtao_enabled_;

  // Resolve the GTAO compute pipeline up front (before frame_uniforms is
  // uploaded via frame.Begin() below) so enable_gtao reflects whether a
  // dispatch will actually be recorded, not just whether GTAO was requested.
  // If compilation fails, enable_gtao must stay 0 so the deferred pass falls
  // back to baked AO instead of reading an AO texture nothing wrote to.
  std::shared_ptr<const CompiledComputePipeline> gtao_pipeline;
  if (run_gtao) {
    gtao_pipeline = pipeline_generator_->GetComputePipeline("compute/gtao");
    if (!gtao_pipeline || !gtao_pipeline->pipeline) {
      spdlog::error("SceneRenderer::Render: failed to compile compute/gtao");
      gtao_pipeline.reset();
    }
  }
  const bool gtao_will_run = run_gtao && gtao_pipeline != nullptr;

  // Directional shadow map (T2): (re)create the depth texture if the
  // configured resolution changed, then fit the light's fixed-coverage
  // ortho frustum to a point ahead of the camera. Computed here (before
  // frame_uniforms is populated below) because light_view_proj must be set
  // BEFORE frame.Begin() uploads the frame UBO -- the shadow pass shares
  // this same per-frame uniform buffer with the rest of the frame.
  if (!shadow_map_.IsValid() ||
      shadow_map_.GetResolution() != shadow_config_.resolution) {
    shadow_map_.CreateTexture(device_, shadow_config_.resolution);
  }
  const glm::vec3 shadow_center_world =
      camera_world_pos +
      camera.direction * (0.5f * shadow_config_.coverage_dmax);
  shadow_map_.UpdateLightMatrices(
      scene.sun_direction, shadow_center_world, shadow_config_.coverage_dmax,
      shadow_config_.resolution, shadow_config_.backward_extension);

  UniformData frame_uniforms{};
  frame_uniforms.view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  frame_uniforms.proj = camera.GetProj();
  // No motion vectors/TAA yet — previous-frame matrices equal current.
  frame_uniforms.view_prev = frame_uniforms.view;
  frame_uniforms.proj_prev = frame_uniforms.proj;
  frame_uniforms.light_view_proj = shadow_map_.GetLightViewProj();
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
  frame_uniforms.enable_gtao = gtao_will_run ? 1u : 0u;
  frame_uniforms.tonemap_mode = 0;     // kClamp (TonemapMode not ported;
                                       // shaders/common/frame.wesl: 0 = kClamp)
  frame_uniforms.output_is_linear =
      (surface_format_ == wgpu::TextureFormat::RGBA16Float ||
       surface_format_ == wgpu::TextureFormat::R32Float)
          ? 1
          : 0;
  frame_uniforms.debug_flags = static_cast<uint32_t>(shadow_debug_mode_);
  // Derived shadow constants (T1: computed here, consumed by T3/T5's
  // shadow-map PCF / contact-shadow ray march). t_size already matches
  // shadow_map_.TexelWorldSize() (both = coverage_dmax / resolution); left
  // as its own computation here rather than reading the ShadowMap back, per
  // T1's plan. coverage_dmax is epsilon-guarded (mirrors
  // ShadowMap::UpdateLightMatrices' guard, shadow_map.cpp) so a
  // misconfigured 0 coverage can't produce a zero/NaN t_size here either;
  // unreachable in-tree (ShadowConfig defaults coverage_dmax = 128).
  const float t_size = std::max(shadow_config_.coverage_dmax, 1e-3f) /
                        static_cast<float>(shadow_config_.resolution);
  // .z is unused: the SSCS ray length used to be precomputed here
  // (1.5*t_size/N_clamp, the grazing worst case) but is now computed
  // per-pixel in contact_shadows.wesl via the real per-pixel NdotL (see
  // shaders/common/frame.wesl's shadowNormalOffsetLength).
  frame_uniforms.shadow_params = glm::vec4(
      t_size, 1.0f / static_cast<float>(shadow_config_.resolution), 0.0f,
      shadow_config_.hard_shadow_debug ? 1.0f : 0.0f);

  FrameContext frame;
  frame.Begin(device_, queue_, frame_uniforms, min_uniform_offset_alignment_);

  // === Pass 0: Shadow depth (T2) — depth-only render of casters from the
  // sun's point of view into shadow_map_'s Depth32Float target (conventional
  // Z: cleared to 1.0 = far, kShadow pipeline compares Less -- see
  // shadow_map.hpp). Guarded on shadow_config_.enable_shadow_map: when off,
  // the map still clears to 1.0 (all lit) but casters are skipped, so T3's
  // (future) sampling reads "no shadow" everywhere instead of stale data.
  // Uses this SAME frame/UBO (light_view_proj was set into frame_uniforms
  // above, before frame.Begin() uploaded it) -- normalmapped.wesl's
  // shadow_pass vertex path reads frame.lightViewProj directly. No color
  // targets: the shadow fragment shader writes nothing but depth. ===
  {
    wgpu::RenderPassDepthStencilAttachment shadow_depth_attachment;
    shadow_depth_attachment.view = shadow_map_.GetDepthView();
    shadow_depth_attachment.depthClearValue = 1.0f;  // conventional-Z: far
    shadow_depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    shadow_depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    shadow_depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    shadow_depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 0;
    desc.colorAttachments = nullptr;
    desc.depthStencilAttachment = &shadow_depth_attachment;

    desc.timestampWrites = gpu_timer_.BeginPass("shadow");
    RenderPassContext pass = frame.BeginRenderPass(desc);
    if (shadow_config_.enable_shadow_map) {
      RenderTexturedMeshes(pass, frame, registry, camera_world_pos,
                           RenderPassType::kShadow, material_instance_cache_);
    }
    pass.End();
  }

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

    desc.timestampWrites = gpu_timer_.BeginPass("gbuffer");
    RenderPassContext pass = frame.BeginRenderPass(desc);
    RenderTexturedMeshes(pass, frame, registry, camera_world_pos,
                         RenderPassType::kGBuffer, material_instance_cache_);
    pass.End();
  }

  // === Pass 1.5: GTAO compute — screen-space ambient occlusion (M6).
  // Runs BETWEEN the G-buffer pass (its depth + normals are the inputs) and
  // the skybox/deferred passes (deferred reads the AO output @7). Skipped when
  // GTAO is off, the device lacks R8Unorm storage, or the pipeline failed to
  // compile (checked above, before frame_uniforms.enable_gtao was set) — the
  // AO texture then keeps its 1.0 clear and enable_gtao=0 makes deferred
  // ignore it. ===
  if (gtao_will_run) {
    // Group 0: frame UBO @0.
    std::array<wgpu::BindGroupEntry, 1> group0_entries{};
    group0_entries[0].binding = 0;
    group0_entries[0].buffer = frame.GetFrameUniformBuffer();
    group0_entries[0].offset = 0;
    group0_entries[0].size = sizeof(UniformData);
    wgpu::BindGroup gtao_group0 = frame.CreateBindGroup(
        gtao_pipeline->bind_group_layouts[0], group0_entries);

    // Group 1: gbuffer depth @0, normals @1, AO storage output @2.
    std::array<wgpu::BindGroupEntry, 3> group1_entries{};
    group1_entries[0].binding = 0;
    group1_entries[0].textureView = gbuffer_.GetDepthView();
    group1_entries[1].binding = 1;
    group1_entries[1].textureView = gbuffer_.GetNormalsView();
    group1_entries[2].binding = 2;
    group1_entries[2].textureView = ao_view_;
    wgpu::BindGroup gtao_group1 = frame.CreateBindGroup(
        gtao_pipeline->bind_group_layouts[1], group1_entries);

    // Guard against a reflected workgroup dim of 0 (unsigned underflow +
    // divide-by-zero in the dispatch-count math below).
    const uint32_t ws_x = std::max(1u, gtao_pipeline->workgroup_size[0]);
    const uint32_t ws_y = std::max(1u, gtao_pipeline->workgroup_size[1]);
    const uint32_t dispatch_x = (width_ + ws_x - 1) / ws_x;
    const uint32_t dispatch_y = (height_ + ws_y - 1) / ws_y;

    wgpu::ComputePassEncoder compute_pass =
        frame.BeginComputePass(gpu_timer_.BeginPass("gtao"));
    compute_pass.SetPipeline(gtao_pipeline->pipeline);
    compute_pass.SetBindGroup(0, gtao_group0, 0, nullptr);
    compute_pass.SetBindGroup(1, gtao_group1, 0, nullptr);
    compute_pass.DispatchWorkgroups(dispatch_x, dispatch_y, 1);
    compute_pass.End();
  }

  // === Pass 1.75: Contact shadows (SSCS, T5) — a fullscreen render pass
  // doing a short view-space ray-march along the sun direction, filling the
  // small Peter-Panning gap the shadow map's normal-offset bias opens near
  // contact points. Runs after the G-buffer pass (reads its depth @1 and
  // normals @2, the latter used (T5-fix) to derive the actual per-pixel
  // normal-offset gap to march) and before deferred lighting (which samples
  // the result @8). Runs every frame and ALWAYS clears contact_shadow_texture_
  // to 1.0 first (the only clear it gets -- see CreateTargets, which creates
  // but no longer separately clears it), so there is never stale contents;
  // the fullscreen ray-march only draws when
  // shadow_config_.enable_contact_shadows (or if the pipeline failed to
  // compile), leaving a bare 1.0 clear otherwise — same enable-gate pattern
  // as Pass 0's shadow map. ===
  {
    RenderPipelineDeclaration decl;
    decl.shader_path = "passes/contact_shadows";

    RenderTargetFormats target_formats = {wgpu::TextureFormat::R8Unorm};
    auto compiled = pipeline_generator_->GetPipeline(decl, target_formats);
    if (!compiled) {
      spdlog::error(
          "SceneRenderer::Render: failed to compile contact_shadows "
          "pipeline");
    }

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = contact_shadow_view_;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {1.0, 1.0, 1.0, 1.0};

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = nullptr;

    desc.timestampWrites = gpu_timer_.BeginPass("contact");
    RenderPassContext pass = frame.BeginRenderPass(desc);
    if (compiled && shadow_config_.enable_contact_shadows) {
      // Group 0: frame UBO @0, gbuffer depth @1, gbuffer normals @2 (T5-fix:
      // the receiver normal, needed to compute per-pixel NdotL and derive
      // the actual normal-offset gap to march).
      std::array<wgpu::BindGroupEntry, 3> entries{};
      entries[0].binding = 0;
      entries[0].buffer = frame.GetFrameUniformBuffer();
      entries[0].offset = 0;
      entries[0].size = sizeof(UniformData);
      entries[1].binding = 1;
      entries[1].textureView = gbuffer_.GetDepthView();
      entries[2].binding = 2;
      entries[2].textureView = gbuffer_.GetNormalsView();

      wgpu::BindGroup bind_group =
          frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

      pass.SetPipeline(compiled->pipeline);
      pass.SetBindGroup(0, bind_group);
      pass.Draw(3);
    }
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

    desc.timestampWrites = gpu_timer_.BeginPass("skybox");
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
      // material @4 (all textureLoad'd — no sampler), shadow map @5 + its
      // comparison sampler @6, GTAO ao @7, contact shadow @8 (textureLoad'd),
      // then IBL: prefiltered env cube @9 + sampler @10, BRDF LUT @11 +
      // sampler @12. Matches shaders/passes/deferred_lighting.wesl's binding
      // declarations. The IBL cube is the prefiltered env when available,
      // else a 1x1 black fallback (so the specular term is zero) — bindings
      // are always valid.
      const bool use_prefiltered = has_prefiltered_ && prefiltered_.IsValid();
      wgpu::TextureView ibl_cube_view =
          use_prefiltered ? prefiltered_.GetView() : fallback_cube_view_;
      wgpu::Sampler ibl_cube_sampler =
          use_prefiltered ? prefiltered_.GetSampler() : fallback_cube_sampler_;

      std::array<wgpu::BindGroupEntry, 13> entries{};
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
      entries[5].binding = 5;
      entries[5].textureView = shadow_map_.GetDepthView();
      entries[6].binding = 6;
      entries[6].sampler = shadow_comparison_sampler_;
      entries[7].binding = 7;
      entries[7].textureView = ao_view_;
      entries[8].binding = 8;
      entries[8].textureView = contact_shadow_view_;
      entries[9].binding = 9;
      entries[9].textureView = ibl_cube_view;
      entries[10].binding = 10;
      entries[10].sampler = ibl_cube_sampler;
      entries[11].binding = 11;
      entries[11].textureView = brdf_lut_.GetView();
      entries[12].binding = 12;
      entries[12].sampler = brdf_lut_.GetSampler();

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

      desc.timestampWrites = gpu_timer_.BeginPass("lighting");
      RenderPassContext pass = frame.BeginRenderPass(desc);
      pass.SetPipeline(compiled->pipeline);
      pass.SetBindGroup(0, bind_group);
      pass.Draw(3);
      pass.End();
    }
  }

  // === Pass 3.5: Volumetric fog (Task: fog rendering) — fill the media
  // cascades (compute) then ray-march + composite into the HDR target, while
  // color is still linear HDR (before tonemap). Reads the G-buffer depth to
  // bound each ray at the scene surface. No-op when fog is disabled. ===
  // Bind the fog generator's emitter + broadphase buffers as the composer's
  // source, so the fill below composes the media from the emitters (animated by
  // the game-fed time) rather than the analytic placeholder shapes.
  if (fog_sim_.IsValid() && volumetric_fog_.GetConfig().enabled) {
    volumetric_fog_.SetFogSources(
        fog_sim_.EmitterBuffer(), fog_sim_.BpCellsBuffer(),
        fog_sim_.BpIndicesBuffer(), fog_sim_.BpMin(), fog_sim_.BpCellSize(),
        fog_sim_.BpNx(), fog_sim_.BpNz(), fog_sim_.EmitterCount(),
        fog_sim_.Time());
  } else {
    volumetric_fog_.ClearFogSources();
  }
  volumetric_fog_.Render(frame, gpu_timer_, camera, hdr_color_view_,
                         gbuffer_.GetDepthView(), shadow_map_.GetDepthView(),
                         shadow_comparison_sampler_, width_, height_);

  // === Pass 3.7: Forward-opaque. Draws ForwardOpaqueRenderable meshes (which
  // bypass the G-buffer and light themselves) into the HDR target with the
  // G-buffer depth Load + write, so they occlude / are occluded by opaque
  // geometry and depth-test the later transparent pass. Skipped when none exist
  // (no such entities in the current scenes; wired so kForwardOpaque is not a
  // silent no-op). ===
  if (registry.view<ForwardOpaqueRenderable>().size() > 0) {
    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = hdr_color_view_;
    color_attachment.loadOp = wgpu::LoadOp::Load;
    color_attachment.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depth_attachment;
    depth_attachment.view = gbuffer_.GetDepthView();
    depth_attachment.depthLoadOp = wgpu::LoadOp::Load;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = &depth_attachment;

    RenderPassContext pass = frame.BeginRenderPass(desc);
    RenderForwardMeshes(pass, frame, registry, camera_world_pos,
                        material_instance_cache_);
    pass.End();
  }

  // === Pass 3.8: Forward-transparent (water). Snapshot the HDR colour (so the
  // water can sample the scene behind it for refraction), then draw
  // ForwardTransparentRenderable meshes into the HDR target with the G-buffer
  // depth bound read-only (test against opaque geometry, never write depth).
  // Skipped when nothing transparent exists (no snapshot cost). ===
  if (registry.view<ForwardTransparentRenderable>().size() > 0) {
    wgpu::TexelCopyTextureInfo copy_src{};
    copy_src.texture = hdr_color_texture_;
    wgpu::TexelCopyTextureInfo copy_dst{};
    copy_dst.texture = hdr_color_copy_texture_;
    wgpu::Extent3D copy_ext = {width_, height_, 1};
    frame.GetEncoder().CopyTextureToTexture(&copy_src, &copy_dst, &copy_ext);

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = hdr_color_view_;
    color_attachment.loadOp = wgpu::LoadOp::Load;
    color_attachment.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depth_attachment;
    depth_attachment.view = gbuffer_.GetDepthView();
    depth_attachment.depthLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Undefined;
    depth_attachment.depthReadOnly = true;
    depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;
    desc.depthStencilAttachment = &depth_attachment;

    RenderPassContext pass = frame.BeginRenderPass(desc);
    const bool use_pref = has_prefiltered_ && prefiltered_.IsValid();
    ForwardEngineResources engine;
    engine.scene_depth = gbuffer_.GetDepthView();
    engine.scene_color = hdr_color_copy_view_;
    engine.scene_color_sampler = linear_clamp_sampler_;
    engine.ibl_prefiltered =
        use_pref ? prefiltered_.GetView() : fallback_cube_view_;
    engine.ibl_sampler =
        use_pref ? prefiltered_.GetSampler() : fallback_cube_sampler_;
    engine.brdf_lut = brdf_lut_.GetView();
    engine.brdf_lut_sampler = brdf_lut_.GetSampler();
    engine.shadow_map = shadow_map_.GetDepthView();
    engine.shadow_sampler = shadow_comparison_sampler_;
    engine.time_seconds = scene.time_seconds;
    RenderForwardTransparentMeshes(pass, frame, registry, camera_world_pos,
                                   material_instance_cache_, engine);
    pass.End();
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

    desc.timestampWrites = gpu_timer_.BeginPass("tonemap");
    RenderPassContext pass = frame.BeginRenderPass(desc);

    if (debug_mode_ != GBufferDebugMode::None) {
      RenderGBufferDebug(pass, frame, *pipeline_generator_, surface_format_,
                         gbuffer_.GetDepthView(), gbuffer_.GetNormalsView(),
                         gbuffer_.GetAlbedoView(), gbuffer_.GetMaterialView(),
                         hdr_color_view_, ao_view_, debug_mode_);
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

  // Resolve this frame's GPU timestamps onto the frame encoder before submit.
  gpu_timer_.ResolveFrame(frame.GetEncoder());

  {
    PROFILE_SCOPE("frame_submit");  // Finish encoding + queue submit
    wgpu::CommandBuffer commands = frame.End();
    queue_.Submit(1, &commands);
  }

  // Advance the async per-pass GPU-timing readback (non-blocking) and print a
  // breakdown when one completes; a cheap no-op when disabled.
  gpu_timer_.EndFrame(std::cerr);
}

}  // namespace badlands
