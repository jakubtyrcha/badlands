#include "renderer.h"
#include <CoreGraphics/CoreGraphics.h>

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
}

void Renderer::set_viewport_size(float w_pts, float h_pts, float backing_scale) {
    if (!layer_) return;

    const float w = w_pts * backing_scale;
    const float h = h_pts * backing_scale;
    if (w <= 0.0f || h <= 0.0f) return; // guard against zero/negative sizes

    layer_->setDrawableSize(CGSizeMake(w, h));
}

void Renderer::render(CA::MetalDrawable* drawable) {
    if (!device_ || !layer_ || !drawable) return;

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
    encoder->endEncoding(); // clear only, nothing drawn yet

    commandBuffer->presentDrawable(drawable);
    commandBuffer->commit();
}

} // namespace sq
