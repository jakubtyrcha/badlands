#include <shapeshifter/ShapeshifterCore.h>

#include "renderer.h"

namespace sq {

struct Editor::Impl {
    Renderer renderer;
};

Editor::Editor() : impl_(new Impl()) {}

Editor* Editor::create() { return new Editor(); }

void Editor::attachLayer(void* caMetalLayer) {
    impl_->renderer.attach_layer(static_cast<CA::MetalLayer*>(caMetalLayer));
}

void Editor::setViewportSize(float widthPts, float heightPts, float backingScale) {
    impl_->renderer.set_viewport_size(widthPts, heightPts, backingScale);
}

void Editor::render(void* caMetalDrawable) {
    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable));
}

} // namespace sq
