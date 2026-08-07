#include "executables/object_viewer/visbuffer_pass.hpp"

#include <cstring>
#include <span>

#include <spdlog/spdlog.h>

namespace badlands::object_viewer {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns. Named here so a reordering in
// visbuffer.slang is a one-line fix rather than a hunt.
constexpr uint32_t kFrameSlot = 0;
constexpr uint32_t kVerticesSlot = 1;
constexpr uint32_t kDrawsSlot = 2;

std::span<const uint8_t> Bytes(const void* p, size_t n) {
  return {static_cast<const uint8_t*>(p), n};
}

}  // namespace

std::unique_ptr<VisbufferPass> VisbufferPass::Create(
    IRhiDevice& device, slang::SlangCompiler& compiler, const SceneMesh& mesh,
    uint32_t width, uint32_t height) {
  // BEFORE anything is allocated: a mesh the packing cannot address is not a
  // mesh this pass can draw, and finding that out after uploading it would put
  // the refusal after the cost.
  if (!ValidateSceneMesh(mesh)) return nullptr;

  auto pass = std::unique_ptr<VisbufferPass>(new VisbufferPass());
  pass->device_ = &device;

  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = "visbuffer", .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("visbuffer::") + entry);
  };
  pass->vs_ = load("vs_visbuffer");
  pass->fs_ = load("fs_visbuffer");
  if (!pass->vs_ || !pass->fs_) return nullptr;

  pass->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = pass->vs_.get(),
       .vertex_entry = "vs_visbuffer",
       .fragment_shader = pass->fs_.get(),
       .fragment_entry = "fs_visbuffer",
       .color_formats = {kVisFormat},
       // REVERSED-Z, and spelled out rather than defaulted. GreaterEqual with a
       // 0.0 clear is the project-wide convention; Less with a 1.0 clear is the
       // shape that renders an empty frame and reads as a missing draw call.
       .depth = {.test_enabled = true,
                 .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = kDepthFormat},
       // Both meshes are wound counter-clockwise seen from outside, so backface
       // culling is what PROVES the winding rather than hiding it -- reversed,
       // the geometry vanishes and reads as a missing draw.
       .cull_mode = CullMode::Back,
       .front_face = FrontFace::Ccw,
       .label = "visbuffer"});
  if (!pass->pipeline_) return nullptr;

  const uint64_t vtx_bytes = mesh.vertices.size() * sizeof(MeshVertex);
  const uint64_t idx_bytes = mesh.indices.size() * sizeof(uint32_t);
  const uint64_t draw_bytes = mesh.draws.size() * sizeof(DrawInfo);
  pass->vertex_buffer_ = device.CreateBuffer(
      {.size = vtx_bytes,
       .usage = BufferUsage::Storage | BufferUsage::CopyDst,
       .label = "mesh_vertices"});
  pass->index_buffer_ = device.CreateBuffer(
      {.size = idx_bytes,
       // Storage AND Index: the raster pass draws with it, and stage 4d's
       // resolve READS it to find a triangle's three vertices.
       .usage = BufferUsage::Index | BufferUsage::Storage |
                BufferUsage::CopyDst,
       .label = "mesh_indices"});
  // ONE ENTRY PER INSTANCE, indexed by the draw slot the visibility buffer
  // packs. The raster pass reads it for the instance offset and the resolve for
  // the material overrides, so both see one array.
  pass->draw_buffer_ = device.CreateBuffer(
      {.size = draw_bytes,
       .usage = BufferUsage::Storage | BufferUsage::CopyDst,
       .label = "draw_info"});
  // A RING, not one buffer: see the comment on alloc_ in the header. Sized far
  // above the 80 bytes a frame needs so the growth path is never taken here.
  pass->alloc_ = FrameAllocator::Create(
      device, {.block_size = 64 * 1024,
               .usage = BufferUsage::Uniform,
               .label = "vis_frame"});
  if (!pass->vertex_buffer_ || !pass->index_buffer_ || !pass->draw_buffer_ ||
      !pass->alloc_) {
    return nullptr;
  }

  pass->vertex_buffer_->Write(0, Bytes(mesh.vertices.data(), vtx_bytes));
  pass->index_buffer_->Write(0, Bytes(mesh.indices.data(), idx_bytes));
  pass->draw_buffer_->Write(0, Bytes(mesh.draws.data(), draw_bytes));
  pass->index_count_ = uint32_t(mesh.indices.size());
  pass->instance_count_ = mesh.InstanceCount();

  if (!pass->BuildTable(pass->alloc_->PrimaryBuffer())) return nullptr;
  if (!pass->BuildTargets(width, height)) return nullptr;
  return pass;
}

bool VisbufferPass::BuildTable(IBuffer* frame_buffer) {
  auto table = device_->CreateBindingTable(
      {.render_pipeline = pipeline_.get(),
       .entries = {{.slot = kFrameSlot,
                    .kind = BindingKind::UniformBuffer,
                    .buffer = frame_buffer,
                    // The ring moves the uniform every frame, so the offset is
                    // dynamic and the table names only the buffer.
                    .dynamic_offset = true},
                   {.slot = kVerticesSlot,
                    .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = vertex_buffer_.get()},
                   // The RASTER pass reads DrawInfo now too, for the instance
                   // offset -- and it must read the same offset the resolve
                   // does, or the barycentrics land on a different triangle.
                   {.slot = kDrawsSlot,
                    .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = draw_buffer_.get()}},
       .label = "visbuffer"});
  if (!table) return false;  // CreateBindingTable logged why
  table_ = std::move(table);
  table_frame_buffer_ = frame_buffer;
  return true;
}

void VisbufferPass::BeginFrame(uint64_t frame_index) {
  alloc_->BeginFrame(frame_index);
}

bool VisbufferPass::BuildTargets(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  visbuffer_ = device_->CreateTexture({.width = width,
                                       .height = height,
                                       .format = kVisFormat,
                                       .usage = TextureUsage::RenderTarget |
                                                TextureUsage::Sampled |
                                                TextureUsage::CopySrc,
                                       .label = "visbuffer"});
  depth_ = device_->CreateTexture({.width = width,
                                   .height = height,
                                   .format = kDepthFormat,
                                   .usage = TextureUsage::DepthStencil |
                                            TextureUsage::Sampled |
                                            TextureUsage::CopySrc,
                                   .label = "vis_depth"});
  if (!visbuffer_ || !depth_) {
    spdlog::error(
        "object_viewer: could not create the visibility-buffer targets at {}x{}",
        width, height);
    return false;
  }
  return true;
}

bool VisbufferPass::Resize(uint32_t width, uint32_t height) {
  if (width == width_ && height == height_) return true;
  return BuildTargets(width, height);
}

void VisbufferPass::SetCamera(const glm::mat4& view, const glm::mat4& proj,
                              glm::vec3 camera_world_pos) {
  frame_.view_proj = proj * view;
  frame_.camera_pos = glm::vec4(camera_world_pos, 0.0f);
}

bool VisbufferPass::AddToGraph(graph::RenderGraph& graph) {
  auto uniform = alloc_->Write(Bytes(&frame_, sizeof(frame_)));
  if (!uniform) {
    spdlog::error("object_viewer: could not allocate the visbuffer frame "
                  "uniform");
    return false;
  }
  frame_offset_ = uint32_t(uniform->offset);
  frame_buffer_ = uniform->buffer;
  // The ring GREW, so the allocation is on a different buffer and the immutable
  // table cannot follow it.
  if (table_frame_buffer_ != frame_buffer_) {
    if (!BuildTable(frame_buffer_)) return false;
  }

  vis_handle_ = graph.ImportTexture(visbuffer_.get(),
                                    ResourceState::Undefined, "visbuffer");
  depth_handle_ = graph.ImportTexture(depth_.get(), ResourceState::Undefined,
                                      "vis_depth");
  vtx_handle_ = graph.ImportBuffer(vertex_buffer_.get(),
                                   ResourceState::Undefined, "mesh_vertices");
  // The INDEX buffer is imported although this pass reads it as an index buffer
  // rather than as storage: the resolve reads it as storage, and importing one
  // buffer twice would give the graph two independent state trackers for it.
  idx_handle_ = graph.ImportBuffer(index_buffer_.get(), ResourceState::Undefined,
                                   "mesh_indices");
  draw_handle_ = graph.ImportBuffer(draw_buffer_.get(), ResourceState::Undefined,
                                    "draw_info");
  auto uni = graph.ImportBuffer(frame_buffer_, ResourceState::Undefined,
                                "vis_frame");
  if (!vis_handle_.IsValid() || !depth_handle_.IsValid() ||
      !vtx_handle_.IsValid() || !idx_handle_.IsValid() ||
      !draw_handle_.IsValid() || !uni.IsValid()) {
    return false;
  }

  // Cleared to kVisEmpty = 0 -- "nothing here" -- and depth to 0.0, which is
  // FAR under reversed-Z. The two clears carry the whole convention between
  // them, and getting either wrong empties the frame.
  const float vis_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  graph.AddRasterPass("visbuffer")
      .ColorTarget(vis_handle_, LoadOp::Clear, StoreOp::Store, vis_clear)
      .DepthTarget(depth_handle_, LoadOp::Clear, StoreOp::Store, 0.0f)
      .Reads(vtx_handle_)
      .Reads(draw_handle_)
      .Reads(uni)
      .Execute([this](const graph::RasterContext& ctx) {
        const uint32_t offsets[1] = {frame_offset_};
        ctx.pass->SetPipeline(pipeline_.get());
        ctx.pass->SetBindingTable(0, table_.get(), offsets);
        ctx.pass->SetIndexBuffer(index_buffer_.get(), IndexFormat::Uint32);
        // ONE draw, N instances: the instance id is the draw slot.
        ctx.pass->DrawIndexed(index_count_, instance_count_);
      });
  return true;
}

}  // namespace badlands::object_viewer
