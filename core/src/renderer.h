#pragma once
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <simd/simd.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <shared_types.h>

namespace sq {

class SceneDocument;
struct Camera;
struct TriangleMesh; // dcsdd.h; kept out of this header, see renderer.cpp

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
    // Modify-mode tangent-frame gizmo. Rebuilds the (tiny, setVertexBytes-only)
    // gizmo vertex vector on every call — 54 verts is cheap enough that
    // caching by param-equality would be over-engineering.
    void set_gizmo(simd_float3 origin, simd_float3 normal, float half_extent);   // shows the gizmo
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
    NS::SharedPtr<MTL::DepthStencilState> depth_test_;   // Less, write ON -- the mesh
    NS::SharedPtr<MTL::DepthStencilState> depth_ignore_; // Always, write OFF -- lines + gizmo

    NS::SharedPtr<MTL::Texture> depth_texture_;
    uint32_t depth_texture_width_ = 0;
    uint32_t depth_texture_height_ = 0;

    NS::SharedPtr<MTL::Buffer> scene_lines_;
    size_t scene_line_vertex_count_ = 0;
    bool scene_lines_dirty_ = true;

    NS::SharedPtr<MTL::Buffer> mesh_vertices_;
    size_t mesh_vertex_count_ = 0;

    bool gizmo_visible_ = false;
    std::vector<LineVertex> gizmo_verts_;
};

} // namespace sq
