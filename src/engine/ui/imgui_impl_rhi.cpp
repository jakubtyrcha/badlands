#include "engine/ui/imgui_impl_rhi.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "engine/rhi/rhi_frame_allocator.hpp"

using namespace badlands;
using namespace badlands::rhi;

namespace {

// Slot numbers are the declaration order of the shader's globals, which is what
// the Slang reflection layer assigns.
constexpr uint32_t kParamsSlot = 0;
constexpr uint32_t kVerticesSlot = 1;
constexpr uint32_t kTextureSlot = 2;
constexpr uint32_t kSamplerSlot = 3;

// Mirrors ImGuiParams in shaders/slang/ui/imgui.slang. One float4, so host and
// device layouts cannot drift.
struct Params {
  float scale_translate[4] = {0, 0, 0, 0};
};
static_assert(sizeof(Params) == 16);

// One GPU texture and the binding table that draws with it. ImGui hands the
// table back as an ImTextureID, so a command naming a texture selects the whole
// binding set in one step.
struct BackendTexture {
  TexturePtr texture;
  BindingTablePtr table;
  // The buffers `table` was built against. A binding table is IMMUTABLE, so
  // when the ring grows past its block the allocation comes back on a
  // different buffer and the table has to be rebuilt to name it -- otherwise
  // the draw reads the primary at offset 0 and renders scrambled geometry with
  // only a one-shot "ring is undersized" warning to explain it.
  IBuffer* params_buffer = nullptr;
  IBuffer* vertex_buffer = nullptr;
};

struct Backend {
  IRhiDevice* device = nullptr;
  ShaderModulePtr vs, fs;
  RenderPipelinePtr pipeline;
  SamplerPtr sampler;
  std::unique_ptr<FrameAllocator> ring;

  uint32_t fb_width = 0;
  uint32_t fb_height = 0;

  // Per-frame, set by AddPass and read inside the pass callback.
  IBuffer* index_buffer = nullptr;   // a slice of the ring, this frame's
  uint64_t index_offset = 0;
  IBuffer* vertex_buffer_used = nullptr;
  uint32_t vertex_offset = 0;
  uint32_t params_offset = 0;
  ImDrawData* draw_data = nullptr;
};

Backend* g = nullptr;

std::span<const uint8_t> Bytes(const void* p, size_t n) {
  return {static_cast<const uint8_t*>(p), n};
}

void DestroyTexture(ImTextureData* tex) {
  if (auto* bt = static_cast<BackendTexture*>(tex->BackendUserData)) {
    IM_DELETE(bt);
    tex->SetTexID(ImTextureID_Invalid);
    tex->BackendUserData = nullptr;
  }
  tex->SetStatus(ImTextureStatus_Destroyed);
}

// Builds (or rebuilds) a texture's binding table against specific buffers.
//
// The uniform and the vertices ride the ring with DYNAMIC offsets, so a table
// normally survives every frame even though the bytes it points at move -- that
// is what the partitioned primary buffer is for. It stops being true the moment
// the ring GROWS: the allocation lands on a different buffer, and an immutable
// table cannot follow it. So the buffers are explicit here and the table is
// rebuilt when they change.
bool BuildTable(BackendTexture& bt, IBuffer* params_buffer,
                IBuffer* vertex_buffer) {
  auto table = g->device->CreateBindingTable(
      {.render_pipeline = g->pipeline.get(),
       .entries = {{.slot = kParamsSlot,
                    .kind = BindingKind::UniformBuffer,
                    .buffer = params_buffer,
                    .dynamic_offset = true},
                   {.slot = kVerticesSlot,
                    .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = vertex_buffer,
                    .dynamic_offset = true},
                   {.slot = kTextureSlot,
                    .kind = BindingKind::SampledTexture,
                    .texture_view = bt.texture->GetDefaultView()},
                   {.slot = kSamplerSlot,
                    .kind = BindingKind::Sampler,
                    .sampler = g->sampler.get()}},
       .label = "imgui"});
  if (!table) return false;
  bt.table = std::move(table);
  bt.params_buffer = params_buffer;
  bt.vertex_buffer = vertex_buffer;
  return true;
}

// Services one ImGui 1.92 texture request. In practice this runs once, for the
// font atlas, on the first frame.
void UpdateTexture(ImTextureData* tex) {
  if (tex->Status == ImTextureStatus_WantCreate) {
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
    auto* bt = IM_NEW(BackendTexture)();
    bt->texture = g->device->CreateTexture(
        {.width = uint32_t(tex->Width),
         .height = uint32_t(tex->Height),
         .format = Format::RGBA8Unorm,
         .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
         .label = "imgui_atlas"});
    if (!bt->texture) {
      spdlog::error("imgui/rhi: could not create a {}x{} texture", tex->Width,
                    tex->Height);
      IM_DELETE(bt);
      return;
    }
    if (!BuildTable(*bt, g->ring->PrimaryBuffer(), g->ring->PrimaryBuffer())) {
      IM_DELETE(bt);
      return;
    }
    tex->SetTexID(ImTextureID(bt));
    tex->BackendUserData = bt;
  }

  if (tex->Status == ImTextureStatus_WantCreate ||
      tex->Status == ImTextureStatus_WantUpdates) {
    auto* bt = static_cast<BackendTexture*>(tex->BackendUserData);
    if (!bt) return;
    // The whole atlas, not the dirty rect: ITexture::Write takes tightly packed
    // rows for a whole mip, and a partial upload would need a region API the
    // RHI does not have. Advertising a sub-rect it cannot do would be worse
    // than uploading a few hundred KB once (rule 4).
    bt->texture->Write(0, 0,
                       Bytes(tex->GetPixels(),
                             size_t(tex->Width) * tex->Height * 4));
    tex->SetStatus(ImTextureStatus_OK);
  }

  if (tex->Status == ImTextureStatus_WantDestroy) DestroyTexture(tex);
}

bool CreateDeviceObjects(const ImGui_ImplRHI_InitInfo& info) {
  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = info.compiler->Get({.module = "imgui", .entry = entry},
                                       slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return info.device->CreateShaderModule(compiled->source,
                                           compiled->reflection,
                                           std::string("imgui::") + entry);
  };
  g->vs = load("vs_imgui");
  g->fs = load("fs_imgui");
  if (!g->vs || !g->fs) return false;

  g->pipeline = info.device->CreateRenderPipeline(
      {.vertex_shader = g->vs.get(),
       .vertex_entry = "vs_imgui",
       .fragment_shader = g->fs.get(),
       .fragment_entry = "fs_imgui",
       .color_formats = {info.target_format},
       // ImGui is entirely alpha-composited -- glyph coverage, window
       // backgrounds, every fade. Without this it draws opaque rectangles.
       .blend_states = {PremultipliedAlphaBlend()},
       .cull_mode = CullMode::None,
       .label = "imgui"});
  if (!g->pipeline) return false;

  g->sampler = info.device->CreateSampler({.mag_filter = FilterMode::Linear,
                                           .min_filter = FilterMode::Linear,
                                           .mip_filter = FilterMode::Linear,
                                           .address_u = AddressMode::ClampToEdge,
                                           .address_v = AddressMode::ClampToEdge,
                                           .label = "imgui"});
  if (!g->sampler) return false;

  // Index too, so the indices get the SAME per-frame slotting the vertices and
  // the params block already had. A single index buffer rewritten at offset 0
  // every frame is overwritten while the 2 other in-flight frames are still
  // reading it -- the exact hazard the ring exists to remove, left in place
  // because SetIndexBuffer takes a buffer rather than a binding table and the
  // shape looked different.
  g->ring = FrameAllocator::Create(
      *info.device,
      {.block_size = 1024 * 1024,
       .usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Index,
       .label = "imgui"});
  return g->ring != nullptr;
}

}  // namespace

bool ImGui_ImplRHI_Init(const ImGui_ImplRHI_InitInfo& info) {
  if (!info.device || !info.compiler) {
    spdlog::error("imgui/rhi: Init needs both a device and a Slang compiler");
    return false;
  }
  delete g;
  g = new Backend();
  g->device = info.device;
  g->fb_width = info.framebuffer_width;
  g->fb_height = info.framebuffer_height;

  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "imgui_impl_rhi";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  // RGBA32 rather than the 8-bit alpha atlas: the RHI has no R8 sampled path
  // wired up, and a format the backend cannot upload is not worth the memory
  // it saves.
  io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

  if (!CreateDeviceObjects(info)) {
    delete g;
    g = nullptr;
    return false;
  }
  return true;
}

bool ImGui_ImplRHI_InitHeadless(const ImGui_ImplRHI_InitInfo& info) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(info.framebuffer_width),
                          float(info.framebuffer_height));
  io.DeltaTime = 1.0f / 60.0f;
  return ImGui_ImplRHI_Init(info);
}

void ImGui_ImplRHI_Shutdown() {
  if (!g) return;
  if (ImGui::GetCurrentContext() != nullptr) {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
      if (tex->BackendUserData != nullptr) DestroyTexture(tex);
    }
  }
  delete g;
  g = nullptr;
}

void ImGui_ImplRHI_NewFrame(uint64_t frame_index) {
  if (g && g->ring) g->ring->BeginFrame(frame_index);
}

uint64_t ImGui_ImplRHI_LastIndexOffset() { return g ? g->index_offset : 0; }
IBuffer* ImGui_ImplRHI_LastIndexBuffer() { return g ? g->index_buffer : nullptr; }
IBuffer* ImGui_ImplRHI_LastVertexBuffer() {
  return g ? g->vertex_buffer_used : nullptr;
}

void ImGui_ImplRHI_SetFramebufferSize(uint32_t width, uint32_t height) {
  if (!g) return;
  g->fb_width = width;
  g->fb_height = height;
}

bool ImGui_ImplRHI_AddPass(ImDrawData* draw_data, graph::RenderGraph& graph,
                           graph::ResourceHandle target) {
  if (!g || !draw_data) return false;
  if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
    return false;
  }

  // Texture create/update/destroy requests, before anything references them.
  if (draw_data->Textures != nullptr) {
    for (ImTextureData* tex : *draw_data->Textures) {
      if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
    }
  }

  const size_t vertex_bytes = size_t(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
  const size_t index_bytes = size_t(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
  if (vertex_bytes == 0 || index_bytes == 0) return false;  // nothing to draw

  // Vertices and indices are FLATTENED across command lists, exactly as ImGui's
  // per-command VtxOffset/IdxOffset assume.
  std::vector<uint8_t> vertices(vertex_bytes);
  std::vector<uint8_t> indices(index_bytes);
  size_t vtx_at = 0, idx_at = 0;
  for (const ImDrawList* list : draw_data->CmdLists) {
    const size_t v = size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
    const size_t i = size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    if (vtx_at + v > vertex_bytes || idx_at + i > index_bytes) break;
    std::memcpy(vertices.data() + vtx_at, list->VtxBuffer.Data, v);
    std::memcpy(indices.data() + idx_at, list->IdxBuffer.Data, i);
    vtx_at += v;
    idx_at += i;
  }

  auto verts = g->ring->Write(vertices);
  if (!verts) {
    spdlog::error("imgui/rhi: could not allocate {} bytes for {} vertices",
                  vertex_bytes, draw_data->TotalVtxCount);
    return false;
  }

  Params params;
  // The standard ImGui projection: y grows DOWN in ImGui space and up in clip
  // space, which is why the y scale is negative.
  const float l = draw_data->DisplayPos.x;
  const float t = draw_data->DisplayPos.y;
  const float w = draw_data->DisplaySize.x;
  const float h = draw_data->DisplaySize.y;
  params.scale_translate[0] = 2.0f / w;
  params.scale_translate[1] = -2.0f / h;
  params.scale_translate[2] = -1.0f - l * (2.0f / w);
  params.scale_translate[3] = 1.0f + t * (2.0f / h);
  auto p = g->ring->Write(Bytes(&params, sizeof(params)));
  if (!p) {
    spdlog::error("imgui/rhi: could not allocate the params block");
    return false;
  }

  // From the ring, so the indices are per-frame like everything else.
  auto idx_alloc = g->ring->Write(indices);
  if (!idx_alloc) {
    spdlog::error("imgui/rhi: could not allocate {} bytes for {} indices",
                  index_bytes, draw_data->TotalIdxCount);
    return false;
  }

  g->vertex_offset = uint32_t(verts->offset);
  g->params_offset = uint32_t(p->offset);
  g->index_buffer = idx_alloc->buffer;
  g->index_offset = idx_alloc->offset;
  g->vertex_buffer_used = verts->buffer;
  g->draw_data = draw_data;

  // The buffers the ALLOCATIONS actually landed on, which is the primary until
  // the ring grows. Every table naming the old pair is rebuilt, and the graph
  // imports these rather than PrimaryBuffer() -- otherwise a growth block is
  // never transitioned and the validation layer reports it Undefined.
  for (ImTextureData* tex : *draw_data->Textures) {
    auto* bt = static_cast<BackendTexture*>(tex->BackendUserData);
    if (!bt || !bt->table) continue;
    if (bt->params_buffer == p->buffer && bt->vertex_buffer == verts->buffer) {
      continue;
    }
    if (!BuildTable(*bt, p->buffer, verts->buffer)) return false;
  }

  auto pass = graph.AddRasterPass("imgui");
  pass.ColorTarget(target, LoadOp::Load, StoreOp::Store);
  // Deduplicated: all three allocations usually share the primary, and
  // importing one buffer three times would emit three transitions for it.
  IBuffer* declared[3] = {nullptr, nullptr, nullptr};
  size_t declared_n = 0;
  for (IBuffer* b : {p->buffer, verts->buffer, idx_alloc->buffer}) {
    if (std::find(declared, declared + declared_n, b) != declared + declared_n) {
      continue;
    }
    declared[declared_n++] = b;
    auto h = graph.ImportBuffer(b, ResourceState::Undefined, "imgui_ring");
    if (!h.IsValid()) return false;
    pass.Reads(h);
  }

  // EVERY TEXTURE THE FRAME SAMPLES, declared. Missing these is not a
  // theoretical gap: the first run of the pixel tests failed on exactly this,
  // with the validation layer naming 'imgui_atlas' as Undefined where
  // SetBindingTable needed ShaderRead. Metal would have rendered it correctly
  // and DX12 would not have.
  if (draw_data->Textures != nullptr) {
    for (ImTextureData* tex : *draw_data->Textures) {
      auto* bt = static_cast<BackendTexture*>(tex->BackendUserData);
      if (!bt || !bt->texture) continue;
      auto handle = graph.ImportTexture(bt->texture.get(),
                                        ResourceState::Undefined, "imgui_atlas");
      if (handle.IsValid()) pass.Reads(handle);
    }
  }

  pass.Execute([](const graph::RasterContext& ctx) {
        ImDrawData* dd = g->draw_data;
        const uint32_t offsets[2] = {g->params_offset, g->vertex_offset};
        ctx.pass->SetPipeline(g->pipeline.get());
        ctx.pass->SetIndexBuffer(g->index_buffer,
                                 sizeof(ImDrawIdx) == 2 ? IndexFormat::Uint16
                                                        : IndexFormat::Uint32,
                                 g->index_offset);

        const ImVec2 clip_off = dd->DisplayPos;
        const ImVec2 clip_scale = dd->FramebufferScale;
        uint32_t global_idx = 0, global_vtx = 0;
        for (const ImDrawList* list : dd->CmdLists) {
          for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
              cmd.UserCallback(list, &cmd);
              continue;
            }
            float min_x = (cmd.ClipRect.x - clip_off.x) * clip_scale.x;
            float min_y = (cmd.ClipRect.y - clip_off.y) * clip_scale.y;
            const float max_x = (cmd.ClipRect.z - clip_off.x) * clip_scale.x;
            const float max_y = (cmd.ClipRect.w - clip_off.y) * clip_scale.y;
            if (max_x <= min_x || max_y <= min_y) continue;
            // Clamped BEFORE the cast: a negative float would wrap to ~4.29e9
            // and produce a scissor rect the backend rejects.
            min_x = std::max(min_x, 0.0f);
            min_y = std::max(min_y, 0.0f);

            uint32_t x = uint32_t(min_x), y = uint32_t(min_y);
            uint32_t rw = uint32_t(max_x - min_x), rh = uint32_t(max_y - min_y);
            if (x + rw > ctx.width) rw = ctx.width > x ? ctx.width - x : 0;
            if (y + rh > ctx.height) rh = ctx.height > y ? ctx.height - y : 0;
            if (rw == 0 || rh == 0) continue;
            ctx.pass->SetScissor(x, y, rw, rh);

            auto* bt = reinterpret_cast<BackendTexture*>(cmd.GetTexID());
            if (!bt || !bt->table) continue;
            ctx.pass->SetBindingTable(0, bt->table.get(), offsets);
            ctx.pass->DrawIndexed(cmd.ElemCount, 1, cmd.IdxOffset + global_idx,
                                  int32_t(cmd.VtxOffset + global_vtx), 0);
          }
          global_idx += uint32_t(list->IdxBuffer.Size);
          global_vtx += uint32_t(list->VtxBuffer.Size);
        }
        // The scissor is pipeline-adjacent state that OUTLIVES this pass on
        // some backends, so it is reset rather than left wherever the last
        // command put it (rule 7).
        ctx.pass->SetScissor(0, 0, ctx.width, ctx.height);
  });
  return true;
}
