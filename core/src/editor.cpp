#include <shapeshifter/ShapeshifterCore.h>

#include <algorithm>
#include <cstring>

#include "camera.h"
#include "camera_controller.h"
#include "picking.h"
#include "renderer.h"
#include "scene.h"

namespace sq {

struct Editor::Impl {
    Renderer renderer;
    SceneDocument scene;
    CameraController controller;
    float viewportWidthPts = 0.0f;  // for pick()'s ray_through_view_point
    float viewportHeightPts = 0.0f; // for CameraController::pan_view and pick()
    int32_t selected = kInvalidNode;
};

Editor::Editor() : impl_(new Impl()) {}

Editor* Editor::create() {
    Editor* editor = new Editor();
    Editor::Impl& impl = *editor->impl_;

    // Scene starts empty — all content comes from spawning (Editor::spawn).
    Camera camera;
    camera.eye = {4.0f, 3.0f, 6.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f; // 60 degrees
    camera.aspect = 1.0f;           // replaced by the first setViewportSize call
    impl.controller = CameraController::from_camera(camera);

    return editor;
}

void Editor::attachLayer(void* caMetalLayer) {
    impl_->renderer.attach_layer(static_cast<CA::MetalLayer*>(caMetalLayer));
}

void Editor::setViewportSize(float widthPts, float heightPts, float backingScale) {
    impl_->renderer.set_viewport_size(widthPts, heightPts, backingScale);
    if (heightPts > 0.0f) {
        impl_->controller.set_aspect(widthPts / heightPts);
        impl_->viewportWidthPts = widthPts;
        impl_->viewportHeightPts = heightPts;
    }
}

void Editor::render(void* caMetalDrawable) {
    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable), impl_->scene, impl_->selected,
                            impl_->controller.to_camera());
}

void Editor::cameraOrbit(float dxPts, float dyPts) {
    impl_->controller.orbit(dxPts, dyPts);
}

void Editor::cameraZoom(float delta) {
    impl_->controller.zoom(delta);
}

void Editor::cameraPan(float dxPts, float dyPts) {
    if (impl_->viewportHeightPts > 0.0f) {
        impl_->controller.pan_view(dxPts, dyPts, impl_->viewportHeightPts);
    }
}

PickResult Editor::pick(float x, float y) const {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return PickResult{kInvalidNode, {}, {}};
    }

    const Ray ray = impl_->controller.to_camera().ray_through_view_point(
        x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const std::optional<PickHit> hit = raycast_scene(impl_->scene, ray);
    if (!hit) {
        return PickResult{kInvalidNode, {}, {}};
    }

    return PickResult{
        hit->node_id,
        Vec3f{hit->hit.point.x, hit->hit.point.y, hit->hit.point.z},
        Vec3f{hit->hit.normal.x, hit->hit.normal.y, hit->hit.normal.z},
    };
}

void Editor::select(int32_t nodeId) {
    impl_->selected = nodeId;
    impl_->renderer.set_scene_lines_dirty(); // selection changes vertex colors
}

int32_t Editor::selectedNode() const {
    return impl_->selected;
}

SpawnResult Editor::spawn(Shape shape, Op op, float x, float y) {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return SpawnResult{kInvalidNode, false};
    }

    const Ray ray = impl_->controller.to_camera().ray_through_view_point(
        x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const std::optional<PickHit> hit = raycast_scene(impl_->scene, ray);

    int32_t id;
    bool snapped;
    if (hit) {
        id = impl_->scene.spawn_snapped(shape, op, hit->hit.point, hit->hit.normal, hit->node_id);
        snapped = true;
    } else {
        id = impl_->scene.spawn_unsnapped(shape, op, ray.origin + ray.dir * kUnsnappedSpawnDistance);
        snapped = false;
    }

    select(id); // same path as select(): sets selected + marks lines dirty
    return SpawnResult{id, snapped};
}

void Editor::nodeName(int32_t nodeId, char* buf, int32_t bufLen) const {
    if (buf == nullptr || bufLen <= 0) {
        return;
    }
    static const std::string kEmpty;
    const Node* node = impl_->scene.find(nodeId);
    const std::string& name = node ? node->name : kEmpty;

    const int32_t maxCopy = bufLen - 1;
    const int32_t n = std::min<int32_t>(maxCopy, static_cast<int32_t>(name.size()));
    if (n > 0) {
        std::memcpy(buf, name.data(), static_cast<size_t>(n));
    }
    buf[n] = '\0';
}

} // namespace sq
