#pragma once
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <simd/simd.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <shared_types.h>

#include "gizmo.h"     // GizmoFrame/GizmoHandle for set_gizmo
#include "sdf_scene.h" // SdfNode -- the raymarch pass's per-frame node scratch buffer

namespace sq {

class SceneDocument;
struct Camera;
struct TriangleMesh; // dcsdd.h; kept out of this header, see renderer.cpp

// Pure, allocation-free per-frame RaymarchUniforms builder -- pulled out of
// Renderer::render() so it's directly unit-testable (see
// tests/core/renderer_tests.cpp) without any Metal device/drawable involved.
// `near`/`far` and `node_count` ride in as plain values rather than a Camera
// reference so this has no dependency beyond shared_types.h's struct layout.
RaymarchUniforms build_raymarch_uniforms(simd_float4x4 view_proj, simd_float4x4 inv_view_proj,
                                          float drawable_width_px, float drawable_height_px,
                                          int32_t node_count, float near, float far);

class Renderer {
public:
    void attach_layer(CA::MetalLayer* layer);                  // borrowed
    void set_viewport_size(float w_pts, float h_pts, float backing_scale);
    void set_scene_lines_dirty();
    // Uploads the reconstructed mesh as the new draw payload. An empty mesh
    // (positions.empty()) drops the existing vertex buffer instead of
    // creating a zero-length one (Metal disallows those) -- see set_mesh's
    // definition for the replace-on-update ownership note.
    void set_mesh(const TriangleMesh& mesh);
    // Modify-mode move gizmo. Rebuilds the (tiny, setVertexBytes-only) gizmo
    // vertex vectors on every call — ~140 verts total is cheap enough that
    // caching by param-equality would be over-engineering. `eye` feeds the
    // camera-facing expansion of the thick handle quads.
    void set_gizmo(const GizmoFrame& frame, GizmoHandle highlighted, simd_float3 eye);   // shows the gizmo
    void hide_gizmo();
    void render(CA::MetalDrawable* drawable, const SceneDocument& doc, int32_t selected_id, const Camera& camera); // borrowed drawable

private:
    // Creates/replaces depth_texture_ iff its size doesn't already match
    // (width, height). Called at render time off the drawable's own texture
    // dimensions -- see render()'s call site comment for why (avoids an
    // ordering hazard against set_viewport_size).
    void ensure_depth_texture(uint32_t width, uint32_t height);

    CA::MetalLayer* layer_ = nullptr;
    NS::SharedPtr<MTL::Device> device_;
    NS::SharedPtr<MTL::CommandQueue> queue_;
    NS::SharedPtr<MTL::Library> library_;
    NS::SharedPtr<MTL::RenderPipelineState> line_pso_;
    NS::SharedPtr<MTL::RenderPipelineState> line_blend_pso_;
    NS::SharedPtr<MTL::RenderPipelineState> mesh_pso_;
    NS::SharedPtr<MTL::RenderPipelineState> raymarch_pso_;
    NS::SharedPtr<MTL::DepthStencilState> depth_test_;   // Less, write ON -- the mesh, the raymarch pass
    NS::SharedPtr<MTL::DepthStencilState> depth_ignore_; // Always, write OFF -- lines + gizmo

    NS::SharedPtr<MTL::Texture> depth_texture_;
    uint32_t depth_texture_width_ = 0;
    uint32_t depth_texture_height_ = 0;

    NS::SharedPtr<MTL::Buffer> scene_lines_;
    size_t scene_line_vertex_count_ = 0;
    bool scene_lines_dirty_ = true;

    NS::SharedPtr<MTL::Buffer> mesh_vertices_;
    size_t mesh_vertex_count_ = 0;

    // Raymarch pass: reused every frame (pack_scene's out-param overload
    // clears + refills it in place) so packing the scene doesn't allocate
    // once capacity has grown to the scene's steady-state node count. This is
    // the CPU-side scratch that feeds raymarch_nodes_'s per-frame upload below.
    std::vector<SdfNode> raymarch_scratch_;

    // Real MTL::Buffer, not setFragmentBytes: the scene is uncapped (sdf.h),
    // so its packed byte size can exceed Metal's 4 KB setFragmentBytes limit.
    // Allocated fresh from raymarch_scratch_ every frame in render() -- no
    // dirty tracking, same as raymarch_scratch_ above (see render()'s
    // allocation site for the ownership/lifetime reasoning).
    NS::SharedPtr<MTL::Buffer> raymarch_nodes_;

    bool gizmo_visible_ = false;
    std::vector<LineVertex> gizmo_grid_verts_;     // thin LINE primitives
    std::vector<LineVertex> gizmo_handle_verts_;   // thick-quad TRIANGLE primitives
};

} // namespace sq
