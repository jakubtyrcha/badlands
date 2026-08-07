#include "rhi_renderer.h"

#include <cassert>

#include <spdlog/spdlog.h>

#include "camera.h"
#include "dcsdd.h" // TriangleMesh
#include "lines.h"
#include "scene.h"
#include "sdf.h" // pack_scene
#include <ground_grid.h>

namespace sq {

using namespace badlands::rhi;
namespace graph = badlands::graph;
namespace slang = badlands::slang;

namespace {

// Slot numbers are the DECLARATION ORDER of each shader's globals, which is what
// Slang's reflection assigns. Named so a reordering in the shader is a one-line
// fix here rather than a hunt for a wrong integer.
//
// The two orders differ, and deliberately so -- they follow each shader as
// written, not a convention imposed across them:
//   raymarch.slang     uniforms, then nodes
//   ground_grid.slang  uniforms
//   mesh/debug_lines   vertices, then uniforms
constexpr uint32_t kRaymarchUniformSlot = 0;
constexpr uint32_t kRaymarchNodesSlot = 1;
constexpr uint32_t kGroundUniformSlot = 0;
constexpr uint32_t kLineVertexSlot = 0;
constexpr uint32_t kLineUniformSlot = 1;

// The viewport's clear colour, unchanged from the metal-cpp path.
constexpr float kClearColor[4] = {0.02f, 0.02f, 0.025f, 1.0f};

std::span<const uint8_t> Bytes(const void* p, size_t n) {
    return {static_cast<const uint8_t*>(p), n};
}

} // namespace

// Pure and allocation-free, so they are unit-tested with no device at all
// (tests/core/renderer_tests.cpp). They came across from the metal-cpp renderer
// unchanged -- packing a uniform struct is the one part of that file the RHI
// port had no reason to touch.
RaymarchUniforms build_raymarch_uniforms(simd_float4x4 view_proj, simd_float4x4 inv_view_proj,
                                          float drawable_width_px, float drawable_height_px,
                                          int32_t node_count, float near, float far) {
    RaymarchUniforms uniforms;
    uniforms.view_proj = view_proj;
    uniforms.inv_view_proj = inv_view_proj;
    uniforms.params0 = simd_make_float4(drawable_width_px, drawable_height_px,
                                         static_cast<float>(node_count), 0.0f);
    uniforms.params1 = simd_make_float4(near, far, 0.0f, 0.0f);
    return uniforms;
}
GroundGridUniforms build_ground_grid_uniforms(simd_float4x4 view_proj, simd_float4x4 inv_view_proj,
                                               float drawable_width_px, float drawable_height_px,
                                               float half_extent, float minor_spacing,
                                               float major_spacing) {
    GroundGridUniforms uniforms;
    uniforms.view_proj = view_proj;
    uniforms.inv_view_proj = inv_view_proj;
    uniforms.params0 = simd_make_float4(drawable_width_px, drawable_height_px, half_extent, 0.0f);
    uniforms.params1 = simd_make_float4(minor_spacing, major_spacing, 0.0f, 0.0f);
    return uniforms;
}

std::unique_ptr<RhiRenderer> RhiRenderer::Create(IRhiDevice& device,
                                                  slang::SlangCompiler& compiler,
                                                  Format color) {
    auto r = std::unique_ptr<RhiRenderer>(new RhiRenderer());
    r->device_ = &device;
    r->compiler_ = &compiler;
    r->color_format_ = color;

    r->pipelines_ = RhiPipelines::Create(device, compiler, color, Format::Depth32Float);
    if (!r->pipelines_) return nullptr;

    // Uniform AND Storage from one ring: a binding table is immutable, so it can
    // only follow the frame if the BUFFER stays put and just the offsets move.
    // Mixing both usages in one allocator is what allows that.
    r->alloc_ = FrameAllocator::Create(
        device, {.block_size = 4 * 1024 * 1024,
                 .usage = BufferUsage::Uniform | BufferUsage::Storage,
                 .label = "shapeshifter"});
    if (!r->alloc_) return nullptr;
    if (!r->EnsureTables()) return nullptr;
    return r;
}

bool RhiRenderer::EnsureTables() {
    IBuffer* ring = alloc_->PrimaryBuffer();
    if (table_buffer_ == ring && table_raymarch_) return true;

    auto two = [&](IRenderPipeline* pipe, uint32_t slot_a, BindingKind kind_a,
                   uint32_t slot_b, BindingKind kind_b, const char* label) {
        return device_->CreateBindingTable(
            {.render_pipeline = pipe,
             .entries = {{.slot = slot_a, .kind = kind_a, .buffer = ring,
                          .dynamic_offset = true},
                         {.slot = slot_b, .kind = kind_b, .buffer = ring,
                          .dynamic_offset = true}},
             .label = label});
    };
    auto one = [&](IRenderPipeline* pipe, uint32_t slot, BindingKind kind,
                   const char* label) {
        return device_->CreateBindingTable(
            {.render_pipeline = pipe,
             .entries = {{.slot = slot, .kind = kind, .buffer = ring,
                          .dynamic_offset = true}},
             .label = label});
    };

    table_raymarch_ = two(pipelines_->raymarch.get(), kRaymarchUniformSlot,
                          BindingKind::UniformBuffer, kRaymarchNodesSlot,
                          BindingKind::ReadOnlyStorageBuffer, "raymarch");
    table_ground_ = one(pipelines_->ground.get(), kGroundUniformSlot,
                        BindingKind::UniformBuffer, "ground");
    table_mesh_ = two(pipelines_->mesh.get(), kLineVertexSlot,
                      BindingKind::ReadOnlyStorageBuffer, kLineUniformSlot,
                      BindingKind::UniformBuffer, "mesh");
    table_lines_ = two(pipelines_->lines.get(), kLineVertexSlot,
                       BindingKind::ReadOnlyStorageBuffer, kLineUniformSlot,
                       BindingKind::UniformBuffer, "lines");
    table_blend_lines_ = two(pipelines_->blend_lines.get(), kLineVertexSlot,
                             BindingKind::ReadOnlyStorageBuffer, kLineUniformSlot,
                             BindingKind::UniformBuffer, "blend_lines");
    table_blend_tris_ = two(pipelines_->blend_tris.get(), kLineVertexSlot,
                            BindingKind::ReadOnlyStorageBuffer, kLineUniformSlot,
                            BindingKind::UniformBuffer, "blend_tris");
    table_origin_ = two(pipelines_->origin.get(), kLineVertexSlot,
                        BindingKind::ReadOnlyStorageBuffer, kLineUniformSlot,
                        BindingKind::UniformBuffer, "origin");

    if (!table_raymarch_ || !table_ground_ || !table_mesh_ || !table_lines_ ||
        !table_blend_lines_ || !table_blend_tris_ || !table_origin_) {
        spdlog::error("shapeshifter: a binding table failed to build");
        return false;
    }
    table_buffer_ = ring;
    return true;
}

void RhiRenderer::AttachLayer(void* ca_metal_layer) {
    // Stored, not used yet. SwiftUI calls this from makeNSView, BEFORE the view
    // has been laid out, so the layer's drawableSize is still zero -- and a
    // zero-sized CAMetalLayer does not return null from nextDrawable, it RAISES
    // an Objective-C exception, which unwinds through C++ and Swift and aborts
    // the process. The old path never met this because Swift handed over a
    // drawable that CAMetalDisplayLink only produces once there is one.
    layer_ = ca_metal_layer;
}

void RhiRenderer::SetViewportSize(uint32_t width_px, uint32_t height_px) {
    if (width_px == 0 || height_px == 0) return;
    if (width_px == width_px_ && height_px == height_px_) return;
    width_px_ = width_px;
    height_px_ = height_px;
    // Deferred, not applied here: a swapchain resizes at ONE point in the frame
    // (after BeginFrame, before Acquire), and this is called from an AppKit
    // notification that can land anywhere.
    pending_resize_ = true;
}

void RhiRenderer::set_scene_lines_dirty() { scene_lines_dirty_ = true; }

void RhiRenderer::set_mesh(const TriangleMesh& mesh) {
    // TriangleMesh's own invariant (dcsdd.h): positions and normals are always
    // the same length, written in lockstep by reconstruct().
    assert(mesh.normals.size() == mesh.positions.size());
    mesh_verts_.clear();
    mesh_verts_.reserve(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        MeshVertex v;
        v.pos = (simd_float4){mesh.positions[i].x, mesh.positions[i].y,
                              mesh.positions[i].z, 1.0f};
        v.normal = (simd_float4){mesh.normals[i].x, mesh.normals[i].y,
                                 mesh.normals[i].z, 0.0f};
        mesh_verts_.push_back(v);
    }
}

void RhiRenderer::set_gizmos(const GizmoFrame& placement, const GizmoFrame& shape,
                             GizmoHit hover, simd_float3 eye, bool show_grid) {
    gizmo_visible_ = true;
    gizmo_grid_verts_.clear();
    gizmo_handle_verts_.clear();

    const bool hovering = hover.handle != GizmoHandle::None;
    const bool on_placement = hovering && hover.slot == GizmoSlot::Placement;
    const bool on_shape = hovering && hover.slot == GizmoSlot::Shape;
    const auto rest_for = [&](bool owns_hover) {
        return (hovering && !owns_hover) ? kGizmoHandleDimAlpha : kGizmoHandleRestAlpha;
    };

    if (show_grid) {
        append_move_gizmo_grid(gizmo_grid_verts_, placement, 12,
                               rest_for(on_placement) / kGizmoHandleRestAlpha);
    }
    // The tether goes down before the handles, so the handles draw over it.
    if (!gizmos_coalesce(placement, shape)) {
        append_anchor_tether(gizmo_handle_verts_, placement.origin, shape.origin,
                             kGizmoPatchBorderHalfWidthFrac * placement.half_extent, eye);
    }
    append_move_gizmo_handles(gizmo_handle_verts_, placement,
                              on_placement ? hover.handle : GizmoHandle::None, eye,
                              rest_for(on_placement));
    append_rotate_gizmo_rings(gizmo_handle_verts_, placement,
                              on_placement ? hover.handle : GizmoHandle::None, eye,
                              rest_for(on_placement));
    append_scale_gizmo_handles(gizmo_handle_verts_, shape,
                               on_shape ? hover.handle : GizmoHandle::None, eye,
                               rest_for(on_shape));
}

void RhiRenderer::hide_gizmo() {
    gizmo_visible_ = false;
    gizmo_grid_verts_.clear();
    gizmo_handle_verts_.clear();
}

void RhiRenderer::set_origin_marker(float height, float half_width,
                                    float pip_half_size, simd_float3 eye) {
    origin_marker_verts_.clear();
    append_origin_marker(origin_marker_verts_, height, half_width, pip_half_size, eye);
}

void RhiRenderer::set_pivot_marker(simd_float3 center, float radius, float half_width,
                                   simd_float3 eye, float alpha) {
    pivot_verts_.clear();
    if (alpha <= 0.0f) return; // faded out: the resting state, so the common path
    simd_float4 color = kColorPivot;
    color.w *= alpha;
    append_pivot_crosshair(pivot_verts_, center, radius, half_width, eye, color);
}

void RhiRenderer::set_focus_preview(simd_float3 center, float half_size, simd_float3 eye) {
    focus_preview_verts_.clear();
    append_focus_dot(focus_preview_verts_, center, half_size, eye, kColorFocusPreview);
}

void RhiRenderer::hide_focus_preview() { focus_preview_verts_.clear(); }

void RhiRenderer::BeginFrame(uint64_t frame_index) { alloc_->BeginFrame(frame_index); }

RhiRenderer::Slice RhiRenderer::UploadLines(const std::vector<LineVertex>& verts) {
    if (verts.empty()) return {};
    auto a = alloc_->Write(Bytes(verts.data(), verts.size() * sizeof(LineVertex)));
    if (!a) {
        spdlog::error("shapeshifter: could not allocate {} line vertices",
                      verts.size());
        return {};
    }
    frame_buffer_ = a->buffer;
    return {uint32_t(a->offset), uint32_t(verts.size())};
}

bool RhiRenderer::BuildFrame(graph::RenderGraph& g, graph::ResourceHandle color,
                             graph::ResourceHandle depth, const SceneDocument& doc,
                             int32_t selected_id, const Camera& camera,
                             uint32_t width_px, uint32_t height_px) {
    if (!EnsureTables()) return false;

    // Scene lines are dirty-tracked; everything else is rebuilt per frame by its
    // setter. Packing the raymarch nodes is NOT dirty-tracked, deliberately: the
    // whole point of the raymarch is that it can never go stale.
    if (scene_lines_dirty_) {
        scene_line_verts_ = build_scene_lines(doc, selected_id, camera.eye);
        scene_lines_dirty_ = false;
    }
    pack_scene(doc, raymarch_scratch_);

    const simd_float4x4 view_proj = camera.view_proj();
    const simd_float4x4 inv_view_proj = simd_inverse(view_proj);
    const float w = float(width_px);
    const float h = float(height_px);

    // --- everything this frame writes, in one pass over the allocator -------
    node_count_ = 0;
    if (!raymarch_scratch_.empty()) {
        auto nodes = alloc_->Write(Bytes(raymarch_scratch_.data(),
                                          raymarch_scratch_.size() * sizeof(SdfNode)));
        const RaymarchUniforms ru = build_raymarch_uniforms(
            view_proj, inv_view_proj, w, h, int32_t(raymarch_scratch_.size()),
            Camera::kNear, Camera::kFar);
        auto uni = alloc_->Write(Bytes(&ru, sizeof(ru)));
        if (!nodes || !uni) {
            spdlog::error("shapeshifter: could not allocate the raymarch frame");
            return false;
        }
        node_offset_ = uint32_t(nodes->offset);
        raymarch_uniform_offset_ = uint32_t(uni->offset);
        node_count_ = uint32_t(raymarch_scratch_.size());
        frame_buffer_ = uni->buffer;
    }

    const GroundGridUniforms gu = build_ground_grid_uniforms(
        view_proj, inv_view_proj, w, h, kGroundHalfExtent, kGroundMinorSpacing,
        kGroundMajorSpacing);
    auto ground_uni = alloc_->Write(Bytes(&gu, sizeof(gu)));
    LineUniforms lu;
    lu.view_proj = view_proj;
    auto line_uni = alloc_->Write(Bytes(&lu, sizeof(lu)));
    if (!ground_uni || !line_uni) {
        spdlog::error("shapeshifter: could not allocate the frame uniforms");
        return false;
    }
    ground_uniform_offset_ = uint32_t(ground_uni->offset);
    line_uniform_offset_ = uint32_t(line_uni->offset);
    frame_buffer_ = line_uni->buffer;

    mesh_slice_ = {};
    if (!mesh_verts_.empty()) {
        auto a = alloc_->Write(Bytes(mesh_verts_.data(),
                                      mesh_verts_.size() * sizeof(MeshVertex)));
        if (!a) return false;
        mesh_slice_ = {uint32_t(a->offset), uint32_t(mesh_verts_.size())};
        frame_buffer_ = a->buffer;
    }
    scene_lines_slice_ = UploadLines(scene_line_verts_);
    gizmo_grid_slice_ = gizmo_visible_ ? UploadLines(gizmo_grid_verts_) : Slice{};
    gizmo_handle_slice_ = gizmo_visible_ ? UploadLines(gizmo_handle_verts_) : Slice{};
    focus_slice_ = UploadLines(focus_preview_verts_);
    pivot_slice_ = UploadLines(pivot_verts_);
    origin_slice_ = UploadLines(origin_marker_verts_);

    // The ring may have grown onto a different buffer mid-frame, which the
    // immutable tables cannot follow.
    if (frame_buffer_ && frame_buffer_ != table_buffer_) {
        spdlog::error(
            "shapeshifter: the frame allocator grew onto a second buffer; the "
            "binding tables cannot follow it within a frame");
        return false;
    }

    // The allocator's buffer is read by every pass, so it is imported once and
    // declared by each -- a buffer nothing declares is never transitioned, and
    // the validation layer reports it Undefined. Undefined on entry because the
    // CPU wrote it outside any pass.
    auto ring = g.ImportBuffer(table_buffer_, ResourceState::Undefined, "ring");
    if (!ring.IsValid()) return false;

    // --- geometry: clears both, writes depth --------------------------------
    auto geometry = g.AddRasterPass("geometry");
    geometry.ColorTarget(color, LoadOp::Clear, StoreOp::Store, kClearColor);
    geometry.DepthTarget(depth, LoadOp::Clear, StoreOp::Store, 0.0f); // reversed-Z far
    geometry.Reads(ring);
    geometry.Execute([this](const graph::RasterContext& ctx) {
        if (node_count_ > 0) {
            // In INCREASING SLOT ORDER, which is what SetBindingTable's span
            // documents. Reversed, each offset lands on the wrong binding and
            // the trace reads uniform bytes as nodes.
            const uint32_t offsets[2] = {raymarch_uniform_offset_, node_offset_};
            ctx.pass->SetPipeline(pipelines_->raymarch.get());
            ctx.pass->SetBindingTable(0, table_raymarch_.get(), offsets);
            ctx.pass->Draw(3); // fullscreen triangle, no vertex buffer
        }
        if (mesh_slice_.count > 0) {
            const uint32_t offsets[2] = {mesh_slice_.offset, line_uniform_offset_};
            ctx.pass->SetPipeline(pipelines_->mesh.get());
            ctx.pass->SetBindingTable(0, table_mesh_.get(), offsets);
            ctx.pass->Draw(mesh_slice_.count);
        }
    });

    // --- ground: tests what geometry wrote, writes no depth -----------------
    auto ground = g.AddRasterPass("ground");
    ground.ColorTarget(color, LoadOp::Load, StoreOp::Store);
    ground.DepthReadOnly(depth);
    ground.Reads(ring);
    ground.Execute([this](const graph::RasterContext& ctx) {
        const uint32_t g_off[1] = {ground_uniform_offset_};
        ctx.pass->SetPipeline(pipelines_->ground.get());
        ctx.pass->SetBindingTable(0, table_ground_.get(), g_off);
        ctx.pass->Draw(3); // fullscreen triangle
        // The origin marker rides in the plate's depth state so all three world
        // axes occlude against the scene identically.
        if (origin_slice_.count > 0) {
            const uint32_t offsets[2] = {origin_slice_.offset, line_uniform_offset_};
            ctx.pass->SetPipeline(pipelines_->origin.get());
            ctx.pass->SetBindingTable(0, table_origin_.get(), offsets);
            ctx.pass->Draw(origin_slice_.count);
        }
    });

    // --- chrome: painter's order, no depth attachment at all ----------------
    auto chrome = g.AddRasterPass("chrome");
    chrome.ColorTarget(color, LoadOp::Load, StoreOp::Store);
    chrome.Reads(ring);
    chrome.Execute([this](const graph::RasterContext& ctx) {
        auto draw = [&](IRenderPipeline* pipe, IBindingTable* table, Slice s) {
            if (s.count == 0) return;
            const uint32_t offsets[2] = {s.offset, line_uniform_offset_};
            ctx.pass->SetPipeline(pipe);
            ctx.pass->SetBindingTable(0, table, offsets);
            ctx.pass->Draw(s.count);
        };
        draw(pipelines_->lines.get(), table_lines_.get(), scene_lines_slice_);
        draw(pipelines_->blend_lines.get(), table_blend_lines_.get(), gizmo_grid_slice_);
        draw(pipelines_->blend_tris.get(), table_blend_tris_.get(), gizmo_handle_slice_);
        draw(pipelines_->blend_tris.get(), table_blend_tris_.get(), focus_slice_);
        // ALWAYS last: the pivot marker is feedback about the gesture being
        // performed right now, so being occluded by the model you are orbiting
        // would defeat it.
        draw(pipelines_->blend_tris.get(), table_blend_tris_.get(), pivot_slice_);
    });
    return true;
}

void RhiRenderer::RenderFrame(const SceneDocument& doc, int32_t selected_id,
                              const Camera& camera) {
    // No size yet means the view has not been laid out, so there is nothing to
    // render into and nothing to render at. The display link starts ticking
    // before that happens.
    if (!layer_ || width_px_ == 0 || height_px_ == 0) {
        return;
    }

    if (!swapchain_) {
        swapchain_ = device_->CreateSwapchain({.native_window = layer_,
                                               .width = width_px_,
                                               .height = height_px_,
                                               .format = color_format_,
                                               .vsync = true,
                                               .label = "shapeshifter"});
        if (!swapchain_) {
            spdlog::error("shapeshifter: could not create the swapchain");
            return;
        }
        pending_resize_ = false; // created at the current size
    }

    device_->BeginFrame();
    // AFTER BeginFrame, so a SKIPPED frame still recycles its allocator slot.
    // Doing it past the Acquire check leaks slots on a minimized window.
    BeginFrame(device_->CurrentFrame());

    if (pending_resize_) {
        swapchain_->Resize(width_px_, height_px_);
        pending_resize_ = false;
    }

    const AcquiredFrame frame = swapchain_->Acquire();
    if (frame.status != AcquireStatus::Ok) {
        // Skip is normal -- minimized, occluded, a display change, a momentarily
        // exhausted drawable pool. Try again next frame.
        device_->EndFrame();
        return;
    }

    // Rebuilt per frame because the drawable is a different texture each time.
    // Caching a graph keyed on a resource that changes every frame is how a
    // stale view gets rendered into.
    graph::RenderGraph g(*device_);
    auto color = g.ImportTexture(frame.view->GetTexture(), ResourceState::Undefined,
                                 "backbuffer");
    auto depth = g.CreateTexture({.width = width_px_,
                                  .height = height_px_,
                                  .format = Format::Depth32Float,
                                  .usage = TextureUsage::DepthStencil,
                                  .label = "depth"});
    if (!color.IsValid() || !depth.IsValid() ||
        !BuildFrame(g, color, depth, doc, selected_id, camera, width_px_, height_px_) ||
        !g.Compile()) {
        device_->EndFrame();
        return;
    }

    auto encoder = device_->CreateCommandEncoder("frame");
    g.Execute(*encoder);
    encoder->Finish();
    device_->Submit(*encoder);
    swapchain_->Present();
    device_->EndFrame();
}

} // namespace sq
