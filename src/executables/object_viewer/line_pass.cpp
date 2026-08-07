#include "executables/object_viewer/line_pass.hpp"

#include <cstring>

#include <spdlog/spdlog.h>

#include "engine/rendering/debug_line_expand.hpp"

namespace badlands::object_viewer {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns. Named here so a reordering in the shader
// is a one-line fix rather than a hunt.
constexpr uint32_t kFrameSlot = 0;
constexpr uint32_t kVerticesSlot = 1;

std::span<const uint8_t> Bytes(const void* p, size_t n) {
  return {static_cast<const uint8_t*>(p), n};
}

}  // namespace

std::unique_ptr<LinePass> LinePass::Create(IRhiDevice& device,
                                           slang::SlangCompiler& compiler,
                                           Format target_format) {
  auto pass = std::unique_ptr<LinePass>(new LinePass());
  pass->device_ = &device;

  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = "thick_line", .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("thick_line::") + entry);
  };
  pass->vs_ = load("vs_lines");
  pass->fs_ = load("fs_lines");
  if (!pass->vs_ || !pass->fs_) return nullptr;

  pass->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = pass->vs_.get(),
       .vertex_entry = "vs_lines",
       .fragment_shader = pass->fs_.get(),
       .fragment_entry = "fs_lines",
       .color_formats = {target_format},
       // The conical antialias fringe is an ALPHA RAMP, so without blending the
       // lines render as hard-edged quads with visible corners.
       //
       // PREMULTIPLIED, because this draws into the UI overlay rather than onto
       // a surface: a layer that accumulates several translucent draws and is
       // composited later only composes associatively in that form.
       .blend_states = {PremultipliedAlphaBlend()},
       .cull_mode = CullMode::None,
       .label = "debug_lines"});
  if (!pass->pipeline_) return nullptr;

  // Uniform AND Storage: the frame block and the vertices come out of the same
  // ring, because a binding table is immutable and can only follow the frame if
  // the BUFFER stays put and just the offsets move.
  pass->alloc_ = FrameAllocator::Create(
      device, {.block_size = 512 * 1024,
               .usage = BufferUsage::Uniform | BufferUsage::Storage,
               .label = "lines"});
  if (!pass->alloc_) return nullptr;

  if (!pass->BuildTable(pass->alloc_->PrimaryBuffer(),
                        pass->alloc_->PrimaryBuffer())) {
    return nullptr;
  }
  return pass;
}

bool LinePass::BuildTable(IBuffer* frame_buffer, IBuffer* vertex_buffer) {
  auto table = device_->CreateBindingTable(
      {.render_pipeline = pipeline_.get(),
       .entries = {{.slot = kFrameSlot,
                    .kind = BindingKind::UniformBuffer,
                    .buffer = frame_buffer,
                    .dynamic_offset = true},
                   {.slot = kVerticesSlot,
                    .kind = BindingKind::ReadOnlyStorageBuffer,
                    .buffer = vertex_buffer,
                    .dynamic_offset = true}},
       .label = "lines"});
  if (!table) return false;
  table_ = std::move(table);
  table_frame_buffer_ = frame_buffer;
  table_vertex_buffer_ = vertex_buffer;
  return true;
}

void LinePass::BeginFrame(uint64_t frame_index) {
  alloc_->BeginFrame(frame_index);
  vertex_count_ = 0;
}

bool LinePass::Upload(const DebugLineBuffer& lines, const glm::mat4& view,
                      const glm::mat4& proj, glm::vec2 screen_size,
                      glm::vec3 camera_world_pos) {
  vertex_count_ = 0;
  // The expansion is the ported code, unchanged: near-plane clipping in clip
  // space before the perspective divide, so no vertex is ever produced from a
  // division by a non-positive w.
  expanded_ =
      ExpandDebugLines(lines, view, proj, screen_size, camera_world_pos);
  if (expanded_.empty()) return true;  // nothing to draw is not a failure

  auto verts = alloc_->Write(
      Bytes(expanded_.data(), expanded_.size() * sizeof(float)));
  if (!verts) {
    spdlog::error("object_viewer: could not allocate {} bytes for {} line "
                  "vertices", expanded_.size() * sizeof(float),
                  expanded_.size() / 8);
    return false;
  }

  LineFrameUniform frame;
  frame.view_proj = proj * view;
  frame.camera_pos = glm::vec4(camera_world_pos, 0.0f);
  auto uniform = alloc_->Write(Bytes(&frame, sizeof(frame)));
  if (!uniform) {
    spdlog::error("object_viewer: could not allocate the line frame uniform");
    return false;
  }

  vertex_offset_ = uint32_t(verts->offset);
  frame_offset_ = uint32_t(uniform->offset);
  vertex_buffer_ = verts->buffer;
  frame_buffer_ = uniform->buffer;
  vertex_count_ = uint32_t(expanded_.size() / 8);

  // The ring GREW, so the allocations are on a different buffer and the
  // immutable table cannot follow them.
  if (table_frame_buffer_ != frame_buffer_ ||
      table_vertex_buffer_ != vertex_buffer_) {
    if (!BuildTable(frame_buffer_, vertex_buffer_)) return false;
  }
  return true;
}

bool LinePass::AddToGraph(graph::RenderGraph& graph,
                          graph::ResourceHandle target, LoadOp load) {
  if (vertex_count_ == 0) return false;

  // The buffers the allocations ACTUALLY landed on, not PrimaryBuffer(): a
  // growth block that is never imported is never transitioned, and the
  // validation layer reports it Undefined. Undefined on entry because the CPU
  // wrote it outside any pass.
  auto pass = graph.AddRasterPass("debug_lines");
  pass.ColorTarget(target, load, StoreOp::Store);
  auto ring = graph.ImportBuffer(vertex_buffer_, ResourceState::Undefined,
                                 "line_ring");
  if (!ring.IsValid()) return false;
  pass.Reads(ring);
  if (frame_buffer_ != vertex_buffer_) {
    auto uni = graph.ImportBuffer(frame_buffer_, ResourceState::Undefined,
                                  "line_uniforms");
    if (!uni.IsValid()) return false;
    pass.Reads(uni);
  }

  pass.Execute([this](const graph::RasterContext& ctx) {
        // In INCREASING SLOT ORDER, which is the contract SetBindingTable's
        // span documents. Reversing them applies each offset to the wrong
        // binding and produces geometry from uniform bytes.
        const uint32_t offsets[2] = {frame_offset_, vertex_offset_};
        ctx.pass->SetPipeline(pipeline_.get());
        ctx.pass->SetBindingTable(0, table_.get(), offsets);
        ctx.pass->Draw(vertex_count_);
      });
  return true;
}

}  // namespace badlands::object_viewer
