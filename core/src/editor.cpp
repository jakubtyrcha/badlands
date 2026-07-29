#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "renderer.h"
#include "scene.h"

namespace sq {

struct Editor::Impl {
    Renderer renderer;
    SceneDocument scene;
    Camera camera;
    int32_t selected = kInvalidNode;
};

Editor::Editor() : impl_(new Impl()) {}

Editor* Editor::create() {
    Editor* editor = new Editor();
    Editor::Impl& impl = *editor->impl_;

    Node cube;
    cube.id = 1;
    cube.shape = Shape::Cube;
    cube.op = Op::Add;
    cube.position = {-0.9f, 0.5f, 0.0f};
    cube.scale = {1.0f, 1.0f, 1.0f};
    impl.scene.add(cube);

    Node sphere;
    sphere.id = 2;
    sphere.shape = Shape::Sphere;
    sphere.op = Op::Subtract;
    sphere.position = {0.9f, 0.5f, 0.0f};
    sphere.scale = {1.0f, 1.0f, 1.0f};
    impl.scene.add(sphere);

    impl.camera.eye = {4.0f, 3.0f, 6.0f};
    impl.camera.target = {0.0f, 0.5f, 0.0f};
    impl.camera.up = {0.0f, 1.0f, 0.0f};
    impl.camera.fov_y_radians = 1.0472f; // 60 degrees
    impl.camera.aspect = 1.0f;           // replaced by the first setViewportSize call

    return editor;
}

void Editor::attachLayer(void* caMetalLayer) {
    impl_->renderer.attach_layer(static_cast<CA::MetalLayer*>(caMetalLayer));
}

void Editor::setViewportSize(float widthPts, float heightPts, float backingScale) {
    impl_->renderer.set_viewport_size(widthPts, heightPts, backingScale);
    if (heightPts > 0.0f) {
        impl_->camera.aspect = widthPts / heightPts;
    }
}

void Editor::render(void* caMetalDrawable) {
    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable), impl_->scene, impl_->selected,
                            impl_->camera);
}

} // namespace sq
