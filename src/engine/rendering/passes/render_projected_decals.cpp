#include "engine/rendering/passes/render_projected_decals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "engine/core/camera.hpp"  // UniformData (frame UBO size)
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/decal_math.hpp"  // ComputeDecalScissor
#include "engine/rendering/gpu_timer.hpp"
#include "engine/rendering/projected_decal.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

namespace {

// GPU mirror of ProjectedDecal. Deliberately vec4-only: a uniform-address-space
// struct array needs a 16-byte-aligned stride, and packing everything into
// vec4s makes the std140 layout trivially correct (and obvious) on both sides.
// MUST match DecalData in shaders/passes/decals.wesl.
struct DecalGpu {
  glm::vec4 center_yaw;          // xyz = centre, w = yaw
  glm::vec4 extents_band_width;  // xy = half extents, z = projector half height, w = line width
  glm::vec4 dash;                // x = corner radius, y = dash length, z = dash gap, w = scroll
  glm::vec4 shape;               // x = DecalShape as float; yzw reserved
  glm::vec4 color_a;
  glm::vec4 color_b;
};

// MUST match DecalUniforms in shaders/passes/decals.wesl.
struct DecalUbo {
  glm::vec4 count_time;  // x = count, y = time seconds; zw reserved
  DecalGpu decals[kMaxProjectedDecals];
};

static_assert(sizeof(DecalGpu) == 96, "DecalGpu must stay 6 x vec4");
static_assert(sizeof(DecalUbo) == 16 + 96 * kMaxProjectedDecals,
              "DecalUbo layout must match shaders/passes/decals.wesl");

}  // namespace

void RenderProjectedDecals(FrameContext& frame, GpuPipelineGenerator& generator,
                           wgpu::TextureFormat color_format,
                           wgpu::TextureView color_target,
                           wgpu::TextureView depth, wgpu::TextureView normals,
                           const ProjectedDecal* decals, uint32_t count,
                           const glm::mat4& view_proj, uint32_t width,
                           uint32_t height, float time_seconds,
                           GpuTimer& gpu_timer) {
  if (decals == nullptr || count == 0 || !color_target || !depth || !normals) {
    return;
  }

  if (count > kMaxProjectedDecals) {
    // Never silently truncate: say what was dropped.
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "RenderProjectedDecals: {} decals submitted, only {} are drawn "
          "(kMaxProjectedDecals) -- the rest are dropped",
          count, kMaxProjectedDecals);
      warned = true;
    }
    count = kMaxProjectedDecals;
  }

  // Shade only the region the decals can reach. An untrustworthy projection
  // falls back to the full viewport; a fully off-screen set draws nothing.
  decal_math::ScissorRect scissor;
  const bool have_scissor = decal_math::ComputeDecalScissor(
      decals, count, view_proj, width, height, scissor);
  if (have_scissor && (scissor.width == 0 || scissor.height == 0)) {
    return;
  }

  RenderPipelineDeclaration decl;
  decl.shader_path = "passes/decals";
  // Fullscreen triangle (default kFullscreen layout); depth is sampled as a
  // texture, not bound as an attachment. Premultiplied alpha: the shader
  // accumulates decals premultiplied, so the target blend is One /
  // OneMinusSrcAlpha.
  decl.blend_enabled = true;
  decl.premultiplied_alpha = true;

  RenderTargetFormats formats = {color_format};
  auto compiled = generator.GetPipeline(decl, formats);
  if (!compiled) {
    spdlog::error("RenderProjectedDecals: failed to compile passes/decals");
    return;
  }

  DecalUbo ubo{};
  ubo.count_time = glm::vec4(static_cast<float>(count), time_seconds, 0.0f, 0.0f);
  for (uint32_t i = 0; i < count; ++i) {
    const ProjectedDecal& d = decals[i];
    DecalGpu& g = ubo.decals[i];
    g.center_yaw = glm::vec4(d.center, d.yaw);
    g.extents_band_width = glm::vec4(d.half_extents.x, d.half_extents.y,
                                     d.projector_half_height, d.line_width);
    g.dash = glm::vec4(d.corner_radius, d.dash_length, d.dash_gap,
                       d.scroll_speed);
    g.shape = glm::vec4(static_cast<float>(d.shape), d.receiver_min_normal_y,
                        d.receiver_max_normal_y, 0.0f);
    g.color_a = d.color_a;
    g.color_b = d.color_b;
  }
  wgpu::Buffer decal_buffer = frame.CreateUniformBuffer(sizeof(ubo), &ubo);

  std::array<wgpu::BindGroupEntry, 4> entries{};
  entries[0].binding = 0;
  entries[0].buffer = frame.GetFrameUniformBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(UniformData);
  entries[1].binding = 1;
  entries[1].textureView = depth;
  entries[2].binding = 2;
  entries[2].textureView = normals;
  entries[3].binding = 3;
  entries[3].buffer = decal_buffer;
  entries[3].offset = 0;
  entries[3].size = sizeof(ubo);

  wgpu::BindGroup bind_group =
      frame.CreateBindGroup(compiled->bind_group_layouts[0], entries);

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = color_target;
  color_attachment.loadOp = wgpu::LoadOp::Load;
  color_attachment.storeOp = wgpu::StoreOp::Store;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;
  desc.depthStencilAttachment = nullptr;
  // Only now, past every early-out: BeginPass reserves a named query slot the
  // moment it is called, so a "decals" row must not be registered unless this
  // pass is actually recorded (its timestamps would never be written).
  desc.timestampWrites = gpu_timer.BeginPass("decals");

  RenderPassContext pass = frame.BeginRenderPass(desc);
  if (have_scissor) {
    pass.SetScissorRect(scissor.x, scissor.y, scissor.width, scissor.height);
  }
  pass.SetPipeline(compiled->pipeline);
  pass.SetBindGroup(0, bind_group);
  pass.Draw(3);
  pass.End();
}

}  // namespace badlands
