#pragma once

// Dear ImGui renderer backend for the native RHI.
//
// WHY NOT imgui_impl_metal / imgui_impl_dx12. Both ship with ImGui and are
// maintained upstream, but each needs a native encoder --
// id<MTLRenderCommandEncoder>, ID3D12GraphicsCommandList* -- handed out of the
// RHI, and src/engine/rhi/CLAUDE.md makes a backend type in an interface header
// a compile error on purpose. It would also put ImGui outside the render graph,
// so ordering it against other passes would stop being the graph's business.
// One backend against the RHI serves Metal, DX12 and Null.
//
// Standard ImGui backend naming and global scope, matching imgui_impl_sdl3 and
// the Dawn backend this replaces -- deliberately NOT in the badlands namespace.
//
// ImGui is the first real caller of THREE RHI features that had only synthetic
// tests: SetScissor (zero users outside the backends), a sampled texture plus
// sampler in a raster pass, and an index buffer fed from the frame ring.

#include <cstdint>

#include <imgui.h>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

struct ImGui_ImplRHI_InitInfo {
  badlands::rhi::IRhiDevice* device = nullptr;
  badlands::slang::SlangCompiler* compiler = nullptr;
  // Must match the attachment ImGui renders into.
  badlands::rhi::Format target_format = badlands::rhi::Format::BGRA8Unorm;
  uint32_t framebuffer_width = 0;
  uint32_t framebuffer_height = 0;
};

// Assumes an ImGui context already exists (imgui_impl_sdl3 makes one).
bool ImGui_ImplRHI_Init(const ImGui_ImplRHI_InitInfo& info);

// Creates the ImGui context too, for a run with no window and no platform
// backend. This is what makes the renderer testable at all: a headless context
// plus a fixed-coordinate draw list plus a pixel readback is a real assertion,
// where "it looked right" is not.
bool ImGui_ImplRHI_InitHeadless(const ImGui_ImplRHI_InitInfo& info);

void ImGui_ImplRHI_Shutdown();

// Recycles this frame's ring slot. Call after IRhiDevice::BeginFrame and before
// ImGui::Render, for the same reason every other per-frame allocator does: a
// skipped frame still owns its slot.
void ImGui_ImplRHI_NewFrame(uint64_t frame_index);

void ImGui_ImplRHI_SetFramebufferSize(uint32_t width, uint32_t height);

// Uploads `draw_data` into this frame's ring and adds a pass drawing it into
// `target`, loading rather than clearing. Returns false when there is nothing
// to draw OR when the upload failed -- the two are distinguished in the log,
// because a silent "nothing to draw" would hide a ring that is too small.
bool ImGui_ImplRHI_AddPass(ImDrawData* draw_data,
                           badlands::graph::RenderGraph& graph,
                           badlands::graph::ResourceHandle target);

// TEST HOOKS. Where this frame's data actually landed in the ring.
//
// They exist because the per-frame hazard they describe is INVISIBLE in a
// rendered image until frames start overlapping, and by then it shows as
// intermittent tearing rather than as a failure. rhi_lab asserts the same
// property for its uniforms via frame_offsets_seen; this is the equivalent.
uint64_t ImGui_ImplRHI_LastIndexOffset();
badlands::rhi::IBuffer* ImGui_ImplRHI_LastIndexBuffer();
badlands::rhi::IBuffer* ImGui_ImplRHI_LastVertexBuffer();
