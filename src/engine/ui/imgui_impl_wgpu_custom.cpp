// Ported from sampo's src/ui/imgui_impl_wgpu_custom.cpp (Task S2.A2).

#include "engine/ui/imgui_impl_wgpu_custom.hpp"

#include <imgui.h>

#include <cstring>
#include <vector>

static wgpu::Device g_Device;
static wgpu::Queue g_Queue;
static wgpu::RenderPipeline g_Pipeline;
static wgpu::BindGroupLayout g_BindGroupLayout;
static wgpu::Sampler g_Sampler;
static wgpu::Buffer g_VertexBuffer;
static wgpu::Buffer g_IndexBuffer;
static wgpu::Buffer g_UniformBuffer;
static size_t g_VertexBufferSize = 0;
static size_t g_IndexBufferSize = 0;
static wgpu::TextureFormat g_RenderTargetFormat =
    wgpu::TextureFormat::Undefined;
static wgpu::TextureFormat g_DepthStencilFormat =
    wgpu::TextureFormat::Undefined;
static uint32_t g_FramebufferWidth = 0;
static uint32_t g_FramebufferHeight = 0;
static uint32_t g_OutputIsLinear = 0;

// Per-texture GPU resources for ImGui 1.92's dynamic texture protocol. ImGui
// owns the ImTextureData (font atlas + any user textures) and drives their
// lifecycle via Status; the backend stores the GPU handles here on
// ImTextureData::BackendUserData. The bind group is the persistent owner of its
// own ref, so the draw loop's transient wgpu::BindGroup wrapper (constructed
// from GetTexID()) is ref-neutral.
struct WgpuBackendTexture {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::BindGroup bind_group;
};

static const char* g_ShaderCode = R"(
struct Uniforms {
    mvp: mat4x4<f32>,
    outputIsLinear: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var texSampler: sampler;
@group(0) @binding(2) var texTexture: texture_2d<f32>;

struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.position = uniforms.mvp * vec4<f32>(in.position, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

// IEC 61966-2-1 sRGB to linear (per-component)
fn srgb_to_linear_component(c: f32) -> f32 {
    if (c <= 0.04045) { return c / 12.92; }
    return pow((c + 0.055) / 1.055, 2.4);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let tex_color = textureSample(texTexture, texSampler, in.uv);
    var result = in.color * tex_color;
    if (uniforms.outputIsLinear == 1u) {
        // ImGui colors are sRGB — convert to linear for RGBA16Float surface
        result = vec4<f32>(
            srgb_to_linear_component(result.r),
            srgb_to_linear_component(result.g),
            srgb_to_linear_component(result.b),
            result.a
        );
    }
    return result;
}
)";

bool ImGui_ImplWGPU_Init(ImGui_ImplWGPU_InitInfo* init_info) {
  g_Device = init_info->Device;
  g_Queue = g_Device.GetQueue();
  g_RenderTargetFormat = init_info->RenderTargetFormat;
  g_DepthStencilFormat = init_info->DepthStencilFormat;
  g_FramebufferWidth = init_info->FramebufferWidth;
  g_FramebufferHeight = init_info->FramebufferHeight;
  g_OutputIsLinear = init_info->OutputIsLinear ? 1 : 0;

  // ImGui 1.92+: tell the core we honor ImTextureData create/update/destroy
  // requests each frame (dynamic font atlas). Without this ImGui falls back to
  // the legacy one-shot atlas path, which this backend no longer implements.
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

  return ImGui_ImplWGPU_CreateDeviceObjects();
}

bool ImGui_ImplWGPU_InitHeadless(ImGui_ImplWGPU_InitInfo* init_info) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize =
      ImVec2(static_cast<float>(init_info->FramebufferWidth),
             static_cast<float>(init_info->FramebufferHeight));
  io.DeltaTime = 1.0f / 60.0f;
  return ImGui_ImplWGPU_Init(init_info);
}

void ImGui_ImplWGPU_SetFramebufferSize(uint32_t width, uint32_t height) {
  g_FramebufferWidth = width;
  g_FramebufferHeight = height;
}

void ImGui_ImplWGPU_Shutdown() { ImGui_ImplWGPU_InvalidateDeviceObjects(); }

void ImGui_ImplWGPU_NewFrame() {}

static void ImGui_ImplWGPU_DestroyTexture(ImTextureData* tex) {
  if (auto* bt = static_cast<WgpuBackendTexture*>(tex->BackendUserData)) {
    bt->bind_group = nullptr;
    bt->view = nullptr;
    if (bt->texture) bt->texture.Destroy();
    bt->texture = nullptr;
    delete bt;
    tex->SetTexID(ImTextureID_Invalid);
    tex->BackendUserData = nullptr;
  }
  tex->SetStatus(ImTextureStatus_Destroyed);
}

// Service one ImGui 1.92 texture request. WantCreate allocates the GPU texture +
// its bind group and stores them on the ImTextureData; WantCreate/WantUpdates
// upload the (sub)region; WantDestroy frees it once ImGui stops referencing it.
static void ImGui_ImplWGPU_UpdateTexture(ImTextureData* tex) {
  if (tex->Status == ImTextureStatus_WantCreate) {
    IM_ASSERT(tex->TexID == ImTextureID_Invalid &&
              tex->BackendUserData == nullptr);
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
    auto* bt = new WgpuBackendTexture();

    wgpu::TextureDescriptor tex_desc;
    tex_desc.size = {static_cast<uint32_t>(tex->Width),
                     static_cast<uint32_t>(tex->Height), 1};
    tex_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    tex_desc.usage =
        wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;
    tex_desc.dimension = wgpu::TextureDimension::e2D;
    bt->texture = g_Device.CreateTexture(&tex_desc);
    bt->view = bt->texture.CreateView();

    // Same single group-0 layout the pipeline expects: uniform + sampler + this
    // texture view.
    wgpu::BindGroupEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].buffer = g_UniformBuffer;
    entries[0].offset = 0;
    entries[0].size = 80;
    entries[1].binding = 1;
    entries[1].sampler = g_Sampler;
    entries[2].binding = 2;
    entries[2].textureView = bt->view;
    wgpu::BindGroupDescriptor bg_desc;
    bg_desc.layout = g_BindGroupLayout;
    bg_desc.entryCount = 3;
    bg_desc.entries = entries;
    bt->bind_group = g_Device.CreateBindGroup(&bg_desc);

    tex->BackendUserData = bt;
    tex->SetTexID((ImTextureID)bt->bind_group.Get());
    // Fall through to upload the pixels for the freshly created texture.
  }

  if (tex->Status == ImTextureStatus_WantCreate ||
      tex->Status == ImTextureStatus_WantUpdates) {
    auto* bt = static_cast<WgpuBackendTexture*>(tex->BackendUserData);
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

    // Full texture on create; the changed block on update. Rows are always the
    // full atlas width apart (we upload from GetPixelsAt with the full pitch).
    const int up_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
    const int up_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
    const int up_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
    const int up_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;

    wgpu::TexelCopyTextureInfo dst;
    dst.texture = bt->texture;
    dst.mipLevel = 0;
    dst.origin = {static_cast<uint32_t>(up_x), static_cast<uint32_t>(up_y), 0};
    dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout layout;
    layout.offset = 0;
    layout.bytesPerRow = static_cast<uint32_t>(tex->Width * tex->BytesPerPixel);
    layout.rowsPerImage = static_cast<uint32_t>(up_h);
    wgpu::Extent3D extent = {static_cast<uint32_t>(up_w),
                             static_cast<uint32_t>(up_h), 1};
    g_Queue.WriteTexture(
        &dst, tex->GetPixelsAt(up_x, up_y),
        static_cast<size_t>(tex->Width * up_h * tex->BytesPerPixel), &layout,
        &extent);
    tex->SetStatus(ImTextureStatus_OK);
  }

  if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
    ImGui_ImplWGPU_DestroyTexture(tex);
}

static void SetupRenderState(ImDrawData* draw_data,
                             wgpu::RenderPassEncoder pass) {
  float L = draw_data->DisplayPos.x;
  float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
  float T = draw_data->DisplayPos.y;
  float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;

  float mvp[4][4] = {
      {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
      {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
      {0.0f, 0.0f, 0.5f, 0.0f},
      {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f},
  };

  g_Queue.WriteBuffer(g_UniformBuffer, 0, mvp, sizeof(mvp));
  g_Queue.WriteBuffer(g_UniformBuffer, 64, &g_OutputIsLinear,
                      sizeof(g_OutputIsLinear));

  pass.SetPipeline(g_Pipeline);
  pass.SetVertexBuffer(0, g_VertexBuffer, 0, g_VertexBufferSize);
  pass.SetIndexBuffer(g_IndexBuffer, wgpu::IndexFormat::Uint16, 0,
                      g_IndexBufferSize);
}

void ImGui_ImplWGPU_RenderDrawData(ImDrawData* draw_data,
                                   wgpu::RenderPassEncoder pass) {
  if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
    return;

  // ImGui 1.92: service texture create/update/destroy requests before drawing.
  // draw_data->Textures usually holds just the font atlas and needs no work
  // after the first frame.
  if (draw_data->Textures != nullptr)
    for (ImTextureData* tex : *draw_data->Textures)
      if (tex->Status != ImTextureStatus_OK) ImGui_ImplWGPU_UpdateTexture(tex);

  // Update vertex and index buffers
  size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
  size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);

  if (vertex_size == 0 || index_size == 0) return;

  // Recreate buffers if needed
  if (g_VertexBufferSize < vertex_size) {
    if (g_VertexBuffer) g_VertexBuffer.Destroy();

    g_VertexBufferSize = vertex_size + 5000 * sizeof(ImDrawVert);
    wgpu::BufferDescriptor desc;
    desc.size = g_VertexBufferSize;
    desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    g_VertexBuffer = g_Device.CreateBuffer(&desc);
  }

  if (g_IndexBufferSize < index_size) {
    if (g_IndexBuffer) g_IndexBuffer.Destroy();

    g_IndexBufferSize = index_size + 10000 * sizeof(ImDrawIdx);
    // Align to 4 bytes for WebGPU
    g_IndexBufferSize = (g_IndexBufferSize + 3) & ~3;
    wgpu::BufferDescriptor desc;
    desc.size = g_IndexBufferSize;
    desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    g_IndexBuffer = g_Device.CreateBuffer(&desc);
  }

  // Upload vertex and index data. Size the staging vectors to the 4-byte-
  // aligned byte counts (not the raw element counts) so the WriteBuffer
  // calls below -- which write the aligned byte size -- never read past the
  // end of the vector (matters for idx_dst when TotalIdxCount is odd, since
  // ImDrawIdx is 2 bytes).
  size_t vtx_size = static_cast<size_t>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
  size_t idx_size = static_cast<size_t>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
  // Align to 4 bytes for WebGPU
  vtx_size = (vtx_size + 3) & ~3;
  idx_size = (idx_size + 3) & ~3;
  std::vector<ImDrawVert> vtx_dst(vtx_size / sizeof(ImDrawVert));
  std::vector<ImDrawIdx> idx_dst(idx_size / sizeof(ImDrawIdx));

  ImDrawVert* vtx_ptr = vtx_dst.data();
  ImDrawIdx* idx_ptr = idx_dst.data();
  size_t vtx_remaining = draw_data->TotalVtxCount;
  size_t idx_remaining = draw_data->TotalIdxCount;

  for (int n = 0; n < draw_data->CmdLists.Size; n++) {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];
    size_t vtx_count = static_cast<size_t>(cmd_list->VtxBuffer.Size);
    size_t idx_count = static_cast<size_t>(cmd_list->IdxBuffer.Size);

    // Safety check: prevent buffer overflow
    if (vtx_count > vtx_remaining || idx_count > idx_remaining) {
      break;
    }

    memcpy(vtx_ptr, cmd_list->VtxBuffer.Data, vtx_count * sizeof(ImDrawVert));
    memcpy(idx_ptr, cmd_list->IdxBuffer.Data, idx_count * sizeof(ImDrawIdx));
    vtx_ptr += vtx_count;
    idx_ptr += idx_count;
    vtx_remaining -= vtx_count;
    idx_remaining -= idx_count;
  }

  g_Queue.WriteBuffer(g_VertexBuffer, 0, vtx_dst.data(), vtx_size);
  g_Queue.WriteBuffer(g_IndexBuffer, 0, idx_dst.data(), idx_size);

  SetupRenderState(draw_data, pass);

  // Render command lists
  ImVec2 clip_off = draw_data->DisplayPos;
  ImVec2 clip_scale = draw_data->FramebufferScale;

  int global_idx_offset = 0;
  int global_vtx_offset = 0;

  for (int n = 0; n < draw_data->CmdLists.Size; n++) {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];

    for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
      const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

      if (pcmd->UserCallback != nullptr) {
        pcmd->UserCallback(cmd_list, pcmd);
      } else {
        // Set scissor
        ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                        (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
        ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                        (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

        if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) continue;

        // Clamp negative clip coords to 0 before the float->uint32_t cast: a
        // negative value would wrap to ~4.29e9, yielding an invalid
        // SetScissorRect (matches the canonical ImGui WGPU backend).
        if (clip_min.x < 0.0f) clip_min.x = 0.0f;
        if (clip_min.y < 0.0f) clip_min.y = 0.0f;

        // Clamp scissor rect to framebuffer bounds
        uint32_t rect_x = (uint32_t)clip_min.x;
        uint32_t rect_y = (uint32_t)clip_min.y;
        uint32_t rect_w = (uint32_t)(clip_max.x - clip_min.x);
        uint32_t rect_h = (uint32_t)(clip_max.y - clip_min.y);

        if (g_FramebufferWidth > 0 && g_FramebufferHeight > 0) {
          if (rect_x + rect_w > g_FramebufferWidth)
            rect_w =
                g_FramebufferWidth > rect_x ? g_FramebufferWidth - rect_x : 0;
          if (rect_y + rect_h > g_FramebufferHeight)
            rect_h =
                g_FramebufferHeight > rect_y ? g_FramebufferHeight - rect_y : 0;
        }

        if (rect_w == 0 || rect_h == 0) continue;

        pass.SetScissorRect(rect_x, rect_y, rect_w, rect_h);

        // Bind texture
        wgpu::BindGroup bind_group =
            (wgpu::BindGroup)(WGPUBindGroup)(ImTextureID)pcmd->GetTexID();
        pass.SetBindGroup(0, bind_group, 0, nullptr);

        pass.DrawIndexed(pcmd->ElemCount, 1,
                         pcmd->IdxOffset + global_idx_offset,
                         pcmd->VtxOffset + global_vtx_offset, 0);
      }
    }
    global_idx_offset += cmd_list->IdxBuffer.Size;
    global_vtx_offset += cmd_list->VtxBuffer.Size;
  }
}

void ImGui_ImplWGPU_InvalidateDeviceObjects() {
  if (g_Pipeline) {
    g_Pipeline = nullptr;
  }
  if (g_BindGroupLayout) {
    g_BindGroupLayout = nullptr;
  }
  if (g_Sampler) {
    g_Sampler = nullptr;
  }
  // Release every dynamic texture (font atlas + any user textures) we created
  // for the 1.92 texture protocol. Guarded so it is safe when called before any
  // context/texture exists.
  if (ImGui::GetCurrentContext() != nullptr) {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
      if (tex->BackendUserData != nullptr) ImGui_ImplWGPU_DestroyTexture(tex);
    }
  }
  if (g_VertexBuffer) {
    g_VertexBuffer.Destroy();
    g_VertexBuffer = nullptr;
  }
  if (g_IndexBuffer) {
    g_IndexBuffer.Destroy();
    g_IndexBuffer = nullptr;
  }
  if (g_UniformBuffer) {
    g_UniformBuffer.Destroy();
    g_UniformBuffer = nullptr;
  }
  g_VertexBufferSize = 0;
  g_IndexBufferSize = 0;
}

bool ImGui_ImplWGPU_CreateDeviceObjects() {
  // Create shader module
  WGPUShaderSourceWGSL wgslDesc = {};
  wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgslDesc.code =
      WGPUStringView{.data = g_ShaderCode, .length = strlen(g_ShaderCode)};

  wgpu::ShaderModuleDescriptor shaderDesc;
  shaderDesc.nextInChain = reinterpret_cast<const wgpu::ChainedStruct*>(&wgslDesc);
  wgpu::ShaderModule shaderModule = g_Device.CreateShaderModule(&shaderDesc);

  // Create uniform buffer (mat4x4 + u32 outputIsLinear, 16-byte aligned)
  wgpu::BufferDescriptor ubDesc;
  ubDesc.size = 80;  // 64 (mat4x4) + 16 (u32 + padding to 16-byte alignment)
  ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  g_UniformBuffer = g_Device.CreateBuffer(&ubDesc);

  // Create sampler
  wgpu::SamplerDescriptor samplerDesc;
  samplerDesc.minFilter = wgpu::FilterMode::Linear;
  samplerDesc.magFilter = wgpu::FilterMode::Linear;
  samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  samplerDesc.addressModeU = wgpu::AddressMode::Repeat;
  samplerDesc.addressModeV = wgpu::AddressMode::Repeat;
  samplerDesc.addressModeW = wgpu::AddressMode::Repeat;
  samplerDesc.maxAnisotropy = 1;
  g_Sampler = g_Device.CreateSampler(&samplerDesc);

  // Create bind group layout
  wgpu::BindGroupLayoutEntry bglEntries[3] = {};

  // Uniform buffer (mat4x4 + outputIsLinear)
  bglEntries[0].binding = 0;
  bglEntries[0].visibility =
      wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  bglEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  bglEntries[0].buffer.minBindingSize = 80;

  // Sampler
  bglEntries[1].binding = 1;
  bglEntries[1].visibility = wgpu::ShaderStage::Fragment;
  bglEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

  // Texture
  bglEntries[2].binding = 2;
  bglEntries[2].visibility = wgpu::ShaderStage::Fragment;
  bglEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
  bglEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;

  wgpu::BindGroupLayoutDescriptor bglDesc;
  bglDesc.entryCount = 3;
  bglDesc.entries = bglEntries;
  g_BindGroupLayout = g_Device.CreateBindGroupLayout(&bglDesc);

  // Create pipeline layout
  wgpu::PipelineLayoutDescriptor plDesc;
  plDesc.bindGroupLayoutCount = 1;
  plDesc.bindGroupLayouts = &g_BindGroupLayout;
  wgpu::PipelineLayout pipelineLayout = g_Device.CreatePipelineLayout(&plDesc);

  // Create pipeline
  wgpu::VertexAttribute vertAttrs[3];
  vertAttrs[0].format = wgpu::VertexFormat::Float32x2;  // Position
  vertAttrs[0].offset = offsetof(ImDrawVert, pos);
  vertAttrs[0].shaderLocation = 0;

  vertAttrs[1].format = wgpu::VertexFormat::Float32x2;  // UV
  vertAttrs[1].offset = offsetof(ImDrawVert, uv);
  vertAttrs[1].shaderLocation = 1;

  vertAttrs[2].format = wgpu::VertexFormat::Unorm8x4;  // Color
  vertAttrs[2].offset = offsetof(ImDrawVert, col);
  vertAttrs[2].shaderLocation = 2;

  wgpu::VertexBufferLayout vertexLayout;
  vertexLayout.arrayStride = sizeof(ImDrawVert);
  vertexLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vertexLayout.attributeCount = 3;
  vertexLayout.attributes = vertAttrs;

  wgpu::RenderPipelineDescriptor pipelineDesc;
  pipelineDesc.layout = pipelineLayout;

  pipelineDesc.vertex.module = shaderModule;
  pipelineDesc.vertex.entryPoint =
      WGPUStringView{.data = "vs_main", .length = 7};
  pipelineDesc.vertex.bufferCount = 1;
  pipelineDesc.vertex.buffers = &vertexLayout;

  wgpu::BlendState blendState;
  blendState.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
  blendState.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blendState.color.operation = wgpu::BlendOperation::Add;
  blendState.alpha.srcFactor = wgpu::BlendFactor::One;
  blendState.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blendState.alpha.operation = wgpu::BlendOperation::Add;

  wgpu::ColorTargetState colorTarget;
  colorTarget.format = g_RenderTargetFormat;
  colorTarget.blend = &blendState;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragmentState;
  fragmentState.module = shaderModule;
  fragmentState.entryPoint = WGPUStringView{.data = "fs_main", .length = 7};
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  pipelineDesc.fragment = &fragmentState;

  // Depth stencil - read depth but don't write
  wgpu::DepthStencilState depthStencil;
  if (g_DepthStencilFormat != wgpu::TextureFormat::Undefined) {
    depthStencil.format = g_DepthStencilFormat;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = wgpu::CompareFunction::Always;
    pipelineDesc.depthStencil = &depthStencil;
  }

  pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
  pipelineDesc.primitive.frontFace = wgpu::FrontFace::CW;

  pipelineDesc.multisample.count = 1;
  pipelineDesc.multisample.mask = ~0u;

  g_Pipeline = g_Device.CreateRenderPipeline(&pipelineDesc);

  // The font atlas texture is no longer created here: under the 1.92 texture
  // protocol ImGui requests it (and any user textures) via ImTextureData, which
  // ImGui_ImplWGPU_UpdateTexture services from RenderDrawData.
  return true;
}
