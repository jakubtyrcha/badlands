#include "engine/ui/ui_renderer.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

namespace badlands {
namespace {

// Must match shaders/ui/ui.wesl's UiFrame block (std140: vec2 + u32 + u32).
struct UiFrameUniform {
  float screen_size[2];
  uint32_t output_is_linear;
  uint32_t output_is_p3;
};
static_assert(sizeof(UiFrameUniform) == 16, "UiFrame must match ui.wesl");

constexpr size_t kFloatsPerVertex = 8;  // pos(2) + uv(2) + color(4)
constexpr size_t kVertsPerQuad = 6;

void UnpackRgba(uint32_t rgba, float* out) {
  out[0] = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
  out[1] = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
  out[2] = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
  out[3] = static_cast<float>(rgba & 0xff) / 255.0f;
}

}  // namespace

UiRenderer::~UiRenderer() {
  if (ui_ctx_) ui_destroy(ui_ctx_);
}

bool UiRenderer::Initialize(wgpu::Device device, wgpu::Queue queue,
                            GpuPipelineGenerator& pipeline_gen,
                            wgpu::TextureFormat surface_format,
                            const char* ttf_path, float px_size,
                            float scale_factor) {
  device_ = device;
  queue_ = queue;

  // File I/O stays on this side of the FFI: the crate takes bytes, not a path,
  // so it carries no cwd assumptions and stays trivially mockable.
  std::vector<uint8_t> ttf;
  {
    std::FILE* f = std::fopen(ttf_path, "rb");
    if (!f) {
      spdlog::error("UiRenderer: cannot open font '{}'", ttf_path);
      return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
      std::fclose(f);
      spdlog::error("UiRenderer: font '{}' is empty", ttf_path);
      return false;
    }
    ttf.resize(static_cast<size_t>(size));
    const size_t read = std::fread(ttf.data(), 1, ttf.size(), f);
    std::fclose(f);
    if (read != ttf.size()) {
      spdlog::error("UiRenderer: short read on font '{}'", ttf_path);
      return false;
    }
  }

  ui_ctx_ = ui_create(ttf.data(), static_cast<uint32_t>(ttf.size()),
                      px_size * scale_factor);
  if (!ui_ctx_) {
    spdlog::error("UiRenderer: ui_create failed for '{}'", ttf_path);
    return false;
  }

  // Atlas: R8 coverage, one mip. WriteTexture has no 256-byte row-alignment
  // requirement (that applies to buffer<->texture copies), so the tightly
  // packed atlas uploads directly.
  std::vector<uint8_t> atlas_pixels;
  {
    UiFontInfo probe{};
    ui_atlas(ui_ctx_, nullptr, 0, &probe);  // fills probe even on capacity error
    const size_t needed =
        static_cast<size_t>(probe.atlas_size) * probe.atlas_size;
    atlas_pixels.resize(needed);
    if (ui_atlas(ui_ctx_, atlas_pixels.data(),
                 static_cast<uint32_t>(atlas_pixels.size()),
                 &font_info_) != UI_OK) {
      spdlog::error("UiRenderer: ui_atlas failed");
      return false;
    }
  }

  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {font_info_.atlas_size, font_info_.atlas_size, 1};
  tex_desc.format = wgpu::TextureFormat::R8Unorm;
  tex_desc.usage =
      wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  tex_desc.mipLevelCount = 1;
  tex_desc.sampleCount = 1;
  wgpu::Texture atlas_tex = device_.CreateTexture(&tex_desc);

  wgpu::TexelCopyTextureInfo dst;
  dst.texture = atlas_tex;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  wgpu::TexelCopyBufferLayout tex_layout;
  tex_layout.bytesPerRow = font_info_.atlas_size;
  tex_layout.rowsPerImage = font_info_.atlas_size;
  wgpu::Extent3D extent = {font_info_.atlas_size, font_info_.atlas_size, 1};
  queue_.WriteTexture(&dst, atlas_pixels.data(), atlas_pixels.size(),
                      &tex_layout, &extent);
  atlas_view_ = atlas_tex.CreateView();

  // Linear filter, single mip -- so the factory default of mipmapFilter=Nearest
  // is correct here rather than a trap.
  wgpu::SamplerDescriptor sampler_desc;
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  sampler_desc.maxAnisotropy = 1;
  sampler_ = device_.CreateSampler(&sampler_desc);

  wgpu::BufferDescriptor ubo_desc;
  ubo_desc.size = sizeof(UiFrameUniform);
  ubo_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  frame_ubo_ = device_.CreateBuffer(&ubo_desc);

  pipeline_gen_ = &pipeline_gen;
  if (!EnsureVariant(surface_format)) {
    spdlog::error("UiRenderer: failed to build the ui/ui pipeline");
    return false;
  }
  return true;
}

UiRenderer::Variant* UiRenderer::EnsureVariant(
    wgpu::TextureFormat target_format) {
  for (auto& [format, variant] : variants_) {
    if (format == target_format) return &variant;
  }
  if (!pipeline_gen_) return nullptr;

  RenderPipelineDeclaration decl;
  decl.shader_path = "ui/ui";
  decl.vertex_layout = VertexLayout::kUiVertex;
  decl.cull_mode = wgpu::CullMode::None;
  decl.blend_enabled = true;  // straight alpha: ui.wesl returns straight alpha
  decl.premultiplied_alpha = false;
  // depth_format stays Undefined -> no depth-stencil state, matching the
  // depth-less overlay pass the app records this into.
  RenderTargetFormats formats = {target_format};
  auto pipeline = pipeline_gen_->GetPipeline(decl, formats);
  if (!pipeline) return nullptr;

  std::array<wgpu::BindGroupEntry, 3> entries{};
  entries[0].binding = 0;
  entries[0].buffer = frame_ubo_;
  entries[0].offset = 0;
  entries[0].size = sizeof(UiFrameUniform);
  entries[1].binding = 1;
  entries[1].textureView = atlas_view_;
  entries[2].binding = 2;
  entries[2].sampler = sampler_;

  Variant variant;
  variant.pipeline = pipeline;
  variant.bind_group = CreateBindGroup(device_, *pipeline, 0, entries);
  // Linear only for float targets; matches how SceneRenderer decides the same
  // flag for its tonemap resolve, so the sRGB branch in ui.wesl agrees with it.
  variant.output_is_linear =
      (target_format == wgpu::TextureFormat::RGBA16Float) ? 1u : 0u;

  variants_.emplace_back(target_format, std::move(variant));
  return &variants_.back().second;
}

void UiRenderer::Prepare(uint32_t width_px, uint32_t height_px,
                         wgpu::TextureFormat target_format,
                         bool output_is_p3) {
  active_ = EnsureVariant(target_format);
  if (!active_ || !frame_ubo_) return;
  const uint32_t is_p3 = output_is_p3 ? 1u : 0u;
  if (width_px == viewport_w_ && height_px == viewport_h_ &&
      active_->output_is_linear == uploaded_is_linear_ &&
      is_p3 == uploaded_is_p3_) {
    return;
  }
  viewport_w_ = width_px;
  viewport_h_ = height_px;
  uploaded_is_linear_ = active_->output_is_linear;
  uploaded_is_p3_ = is_p3;
  UiFrameUniform u{};
  u.screen_size[0] = static_cast<float>(width_px);
  u.screen_size[1] = static_cast<float>(height_px);
  u.output_is_linear = active_->output_is_linear;
  u.output_is_p3 = is_p3;
  queue_.WriteBuffer(frame_ubo_, 0, &u, sizeof(u));
}

void UiRenderer::EnsureVertexCapacity(size_t bytes) {
  if (bytes <= vertex_capacity_bytes_ && vertex_buffer_) return;
  // Grow by doubling so a steady-state HUD stops reallocating after a few
  // frames (mirrors FrameContext::ReserveDynamicUniform).
  size_t capacity = vertex_capacity_bytes_ ? vertex_capacity_bytes_ : 4096;
  while (capacity < bytes) capacity *= 2;
  wgpu::BufferDescriptor desc;
  desc.size = capacity;
  desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  vertex_buffer_ = device_.CreateBuffer(&desc);
  vertex_capacity_bytes_ = capacity;
}

void UiRenderer::SetQuads(const UiQuad* quads, uint32_t count) {
  vertex_count_ = 0;
  vertices_.clear();
  if (!quads || count == 0 || !ready()) return;

  vertices_.reserve(count * kVertsPerQuad * kFloatsPerVertex);
  for (uint32_t i = 0; i < count; ++i) {
    const UiQuad& q = quads[i];
    float color[4];
    UnpackRgba(q.rgba, color);

    // Corner order (tl, bl, br, tr) then two triangles: 0-1-2, 0-2-3.
    const float xs[4] = {q.x, q.x, q.x + q.w, q.x + q.w};
    const float ys[4] = {q.y, q.y + q.h, q.y + q.h, q.y};
    const float us[4] = {q.u0, q.u0, q.u1, q.u1};
    const float vs[4] = {q.v0, q.v1, q.v1, q.v0};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k : order) {
      vertices_.push_back(xs[k]);
      vertices_.push_back(ys[k]);
      vertices_.push_back(us[k]);
      vertices_.push_back(vs[k]);
      vertices_.insert(vertices_.end(), color, color + 4);
    }
  }

  const size_t bytes = vertices_.size() * sizeof(float);
  EnsureVertexCapacity(bytes);
  // queue.WriteBuffer is ordered against the subsequent submit, so writing and
  // drawing in the same frame needs no fence.
  queue_.WriteBuffer(vertex_buffer_, 0, vertices_.data(), bytes);
  vertex_count_ = static_cast<uint32_t>(count * kVertsPerQuad);
}

void UiRenderer::Draw(wgpu::RenderPassEncoder& pass) {
  if (!active_ || vertex_count_ == 0 || !vertex_buffer_) return;
  pass.SetPipeline(active_->pipeline->pipeline);
  pass.SetBindGroup(0, active_->bind_group);
  pass.SetVertexBuffer(0, vertex_buffer_, 0,
                       vertex_count_ * kFloatsPerVertex * sizeof(float));
  pass.Draw(vertex_count_);
}

}  // namespace badlands
