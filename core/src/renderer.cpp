#include "renderer.h"
#include <CoreGraphics/CoreGraphics.h>
#include <cassert>
#include <cstdio>

#include "camera.h"
#include "lines.h"
#include "scene.h"

namespace sq {

void Renderer::attach_layer(CA::MetalLayer* layer) {
    layer_ = layer;

    device_ = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    queue_ = NS::TransferPtr(device_->newCommandQueue());

    layer_->setDevice(device_.get());
    layer_->setPixelFormat(MTL::PixelFormatRGBA16Float);
    layer_->setFramebufferOnly(true);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    layer_->setColorspace(cs);
    CGColorSpaceRelease(cs); // the layer retains it

    library_ = NS::TransferPtr(device_->newDefaultLibrary());
    assert(library_ && "failed to load default Metal library");

    NS::SharedPtr<MTL::Function> vertex_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("debug_line_vertex", NS::UTF8StringEncoding)));
    NS::SharedPtr<MTL::Function> fragment_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("debug_line_fragment", NS::UTF8StringEncoding)));
    assert(vertex_fn && "debug_line_vertex missing from default.metallib");
    assert(fragment_fn && "debug_line_fragment missing from default.metallib");

    NS::SharedPtr<MTL::RenderPipelineDescriptor> pipelineDesc =
        NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    pipelineDesc->setVertexFunction(vertex_fn.get());
    pipelineDesc->setFragmentFunction(fragment_fn.get());
    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);
    // No depth attachment format: the wireframe pass draws depth-ignored, by design.

    NS::Error* error = nullptr;
    line_pso_ = NS::TransferPtr(device_->newRenderPipelineState(pipelineDesc.get(), &error));
    if (!line_pso_) {
        fprintf(stderr, "failed to create line_pso_: %s\n",
                error ? error->localizedDescription()->utf8String() : "unknown error");
        assert(false && "failed to create line render pipeline state");
    }
}

void Renderer::set_viewport_size(float w_pts, float h_pts, float backing_scale) {
    if (!layer_) return;

    const float w = w_pts * backing_scale;
    const float h = h_pts * backing_scale;
    if (w <= 0.0f || h <= 0.0f) return; // guard against zero/negative sizes

    layer_->setDrawableSize(CGSizeMake(w, h));
}

void Renderer::set_scene_lines_dirty() {
    scene_lines_dirty_ = true;
}

void Renderer::render(CA::MetalDrawable* drawable, const SceneDocument& doc, int32_t selected_id,
                       const Camera& camera) {
    if (!device_ || !layer_ || !drawable) return;

    if (scene_lines_dirty_) {
        std::vector<LineVertex> vertices = build_scene_lines(doc, selected_id);
        scene_line_vertex_count_ = vertices.size();
        if (!vertices.empty()) {
            // Metal disallows ever creating a zero-length buffer, hence the guard;
            // replacing scene_lines_ here is safe even mid-flight — the in-flight
            // command buffer retains the old MTL::Buffer via ARC.
            scene_lines_ = NS::TransferPtr(device_->newBuffer(
                vertices.data(), vertices.size() * sizeof(LineVertex), MTL::ResourceStorageModeShared));
        }
        scene_lines_dirty_ = false;
    }

    MTL::CommandBuffer* commandBuffer = queue_->commandBuffer();

    // alloc()->init() returns a +1 owned pointer (not autoreleased), so it is
    // wrapped in a SharedPtr rather than left to leak once per frame.
    NS::SharedPtr<MTL::RenderPassDescriptor> passDescriptor =
        NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

    MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
        passDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(drawable->texture());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setClearColor(MTL::ClearColor(0.02, 0.02, 0.025, 1.0));

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(passDescriptor.get());
    if (scene_line_vertex_count_ > 0) {
        LineUniforms uniforms;
        uniforms.view_proj = camera.view_proj();
        // Buffer indices 0/1 are hardcoded to match the shader's [[buffer(0)]]/[[buffer(1)]] — no reflection.
        encoder->setRenderPipelineState(line_pso_.get());
        encoder->setVertexBuffer(scene_lines_.get(), 0, 0);
        encoder->setVertexBytes(&uniforms, sizeof(LineUniforms), 1);
        // NS::UInteger(0), not the literal 0: a bare 0 is also a null-pointer constant, which
        // makes it ambiguous against the (PrimitiveType, const Buffer*, offset) overload.
        encoder->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), scene_line_vertex_count_);
    }
    encoder->endEncoding();

    commandBuffer->presentDrawable(drawable);
    commandBuffer->commit();
}

} // namespace sq
