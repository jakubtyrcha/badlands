#pragma once

// The editor's renderer, on the engine's RHI and render graph.
//
// THE FRAME IS THREE PASSES, where the metal-cpp path was one encoder with ten
// draws that switched pipeline and depth-stencil state between them. The RHI
// bakes depth state and topology into the pipeline, so what varied per draw now
// varies per pipeline, and what remains is grouped by depth behaviour:
//
//   geometry  colour CLEAR + depth CLEAR/write   raymarch, mesh
//   ground    colour LOAD  + depth READ-ONLY     ground plate, origin marker
//   chrome    colour LOAD  + NO depth            lines, gizmo, focus, pivot
//
// The chrome pass carries no depth attachment at all, which is what lets its
// pipelines declare no depth format -- Metal validation requires the two to
// agree, and "both say none" satisfies it as surely as "both say Depth32Float".
//
// Draw order inside each pass is the metal-cpp order, unchanged: it is painter's
// order for the chrome, and the pivot marker is last because it is feedback
// about the gesture you are performing right now.

#include <cstdint>
#include <memory>
#include <vector>

#include <simd/simd.h>

#include "engine/graph/render_graph.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"

#include "gizmo.h"
#include "rhi_pipelines.h"
#include <shared_types.h>
#include <sdf_scene.h>

namespace sq {

class SceneDocument;
struct Camera;
struct TriangleMesh;

// Pure, allocation-free per-frame uniform builders, unchanged from the
// metal-cpp renderer and still unit-tested without a device.
RaymarchUniforms build_raymarch_uniforms(simd_float4x4 view_proj, simd_float4x4 inv_view_proj,
                                          float drawable_width_px, float drawable_height_px,
                                          int32_t node_count, float near, float far);

GroundGridUniforms build_ground_grid_uniforms(simd_float4x4 view_proj, simd_float4x4 inv_view_proj,
                                               float drawable_width_px, float drawable_height_px,
                                               float half_extent, float minor_spacing,
                                               float major_spacing);

class RhiRenderer {
public:
    // `device` and `compiler` are BORROWED and outlive this. Editor owns them,
    // so the test path and the app path construct a renderer the same way --
    // the alternative, letting AttachLayer create a device, gives the two paths
    // different construction and only one of them gets tested.
    static std::unique_ptr<RhiRenderer> Create(badlands::rhi::IRhiDevice& device,
                                               badlands::slang::SlangCompiler& compiler,
                                               badlands::rhi::Format color);

    // Takes the view's CAMetalLayer -- the same pointer Swift already hands to
    // Editor::attachLayer, borrowed. The swapchain is NOT created here: SwiftUI
    // attaches the layer before it lays the view out, so the size is still zero
    // at this point, and a zero-sized CAMetalLayer throws from nextDrawable
    // rather than returning null. It is created on the first real size instead.
    void AttachLayer(void* ca_metal_layer);
    void SetViewportSize(uint32_t width_px, uint32_t height_px);

    // Per-frame scene inputs, same contract as the metal-cpp renderer.
    void set_scene_lines_dirty();
    void set_mesh(const TriangleMesh& mesh);
    void set_gizmos(const GizmoFrame& placement, const GizmoFrame& shape, GizmoHit hover,
                    simd_float3 eye, bool show_grid);
    void hide_gizmo();
    void set_pivot_marker(simd_float3 center, float radius, float half_width, simd_float3 eye,
                          float alpha);
    void set_focus_preview(simd_float3 center, float half_size, simd_float3 eye);
    void hide_focus_preview();
    void set_origin_marker(float height, float half_width, float pip_half_size, simd_float3 eye);

    // Acquires, records the three passes, presents. A Skip from Acquire is a
    // normal frame on a minimized or occluded window, not a failure.
    void RenderFrame(const SceneDocument& doc, int32_t selected_id, const Camera& camera);

    // Recycles this frame's allocator slot. Called from RenderFrame, and
    // directly by tests that drive the graph without a swapchain.
    void BeginFrame(uint64_t frame_index);

    // Declares the three passes into `graph`, against caller-supplied colour and
    // depth handles. Split out of RenderFrame so a test can execute the frame on
    // the Null backend with no swapchain and no display.
    bool BuildFrame(badlands::graph::RenderGraph& graph,
                    badlands::graph::ResourceHandle color,
                    badlands::graph::ResourceHandle depth,
                    const SceneDocument& doc, int32_t selected_id,
                    const Camera& camera, uint32_t width_px, uint32_t height_px);

private:
    // One draw's worth of geometry in the frame allocator: where it landed and
    // how many vertices it is. count == 0 means "skip this draw".
    struct Slice {
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    // Writes `verts` into the frame allocator. Returns a zero-count Slice for an
    // empty input, which every draw site treats as "nothing to draw" -- the same
    // meaning the metal-cpp path gave a null buffer.
    Slice UploadLines(const std::vector<LineVertex>& verts);
    bool EnsureTables();

    badlands::rhi::IRhiDevice* device_ = nullptr;
    badlands::slang::SlangCompiler* compiler_ = nullptr;
    std::unique_ptr<RhiPipelines> pipelines_;
    std::unique_ptr<badlands::rhi::FrameAllocator> alloc_;
    void* layer_ = nullptr; // borrowed; the swapchain is built from it lazily
    badlands::rhi::SwapchainPtr swapchain_;
    badlands::rhi::Format color_format_ = badlands::rhi::Format::RGBA16Float;

    // One table per pipeline. Pipelines sharing a shader share a layout, but the
    // table is created against a pipeline, so it is built per pipeline rather
    // than assumed interchangeable.
    badlands::rhi::BindingTablePtr table_raymarch_;
    badlands::rhi::BindingTablePtr table_ground_;
    badlands::rhi::BindingTablePtr table_mesh_;
    badlands::rhi::BindingTablePtr table_lines_;
    badlands::rhi::BindingTablePtr table_blend_lines_;
    badlands::rhi::BindingTablePtr table_blend_tris_;
    badlands::rhi::BindingTablePtr table_origin_;
    // The buffer the tables were built against. A table is immutable, so when
    // the allocator's ring grows onto a different buffer they must be rebuilt.
    badlands::rhi::IBuffer* table_buffer_ = nullptr;

    uint32_t width_px_ = 0;
    uint32_t height_px_ = 0;
    bool pending_resize_ = false;

    // Per-frame offsets into the allocator, filled by BuildFrame.
    uint32_t raymarch_uniform_offset_ = 0;
    uint32_t node_offset_ = 0;
    uint32_t node_count_ = 0;
    uint32_t ground_uniform_offset_ = 0;
    uint32_t line_uniform_offset_ = 0;
    Slice mesh_slice_;
    Slice scene_lines_slice_;
    Slice gizmo_grid_slice_;
    Slice gizmo_handle_slice_;
    Slice focus_slice_;
    Slice pivot_slice_;
    Slice origin_slice_;
    badlands::rhi::IBuffer* frame_buffer_ = nullptr;

    // CPU-side scene state, same as the metal-cpp renderer held.
    std::vector<SdfNode> raymarch_scratch_;
    std::vector<LineVertex> scene_line_verts_;
    bool scene_lines_dirty_ = true;
    std::vector<MeshVertex> mesh_verts_;
    std::vector<LineVertex> gizmo_grid_verts_;
    std::vector<LineVertex> gizmo_handle_verts_;
    std::vector<LineVertex> focus_preview_verts_;
    std::vector<LineVertex> pivot_verts_;
    std::vector<LineVertex> origin_marker_verts_;
    bool gizmo_visible_ = false;
};

} // namespace sq
