#pragma once
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <cstddef>
#include <cstdint>

namespace sq {

class SceneDocument;
struct Camera;

class Renderer {
public:
    void attach_layer(CA::MetalLayer* layer);                  // borrowed
    void set_viewport_size(float w_pts, float h_pts, float backing_scale);
    void set_scene_lines_dirty();
    void render(CA::MetalDrawable* drawable, const SceneDocument& doc, int32_t selected_id, const Camera& camera); // borrowed drawable

private:
    CA::MetalLayer* layer_ = nullptr;
    NS::SharedPtr<MTL::Device> device_;
    NS::SharedPtr<MTL::CommandQueue> queue_;
    NS::SharedPtr<MTL::Library> library_;
    NS::SharedPtr<MTL::RenderPipelineState> line_pso_;
    NS::SharedPtr<MTL::Buffer> scene_lines_;
    size_t scene_line_vertex_count_ = 0;
    bool scene_lines_dirty_ = true;
};

} // namespace sq
