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

    bool gizmo_visible = false;
    struct {
        bool active = false;
        simd_float3 plane_point, plane_normal, start_pos, start_hit, start_snap_point;
    } drag;
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
    const Camera camera = impl_->controller.to_camera();

    // Per-frame gizmo push: core owns all the plane math, so the renderer
    // just gets an origin/normal/extent to draw a grid at.
    const Node* selectedNode = impl_->gizmo_visible ? impl_->scene.find(impl_->selected) : nullptr;
    if (selectedNode != nullptr) {
        const simd_float3 camera_forward = simd_normalize(camera.target - camera.eye);
        const DragPlane dp = drag_plane_for_node(*selectedNode, camera_forward);
        const float half_extent = std::max(
            1.5f * std::max({selectedNode->scale.x, selectedNode->scale.y, selectedNode->scale.z}), 1.5f);
        impl_->renderer.set_gizmo(true, dp.point, dp.normal, half_extent);
    } else {
        impl_->renderer.set_gizmo(false, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.5f);
    }

    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable), impl_->scene, impl_->selected, camera);
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

void Editor::setGizmoVisible(bool visible) {
    impl_->gizmo_visible = visible;
}

void Editor::beginDrag(float x, float y) {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return;
    }
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return; // no valid selection: drag does not activate
    }

    const Camera camera = impl_->controller.to_camera();
    const simd_float3 camera_forward = simd_normalize(camera.target - camera.eye);
    // Captured NOW: the plane is fixed for the whole drag, even though the
    // node (and, for a snapped node, its snap fields) moves as the drag
    // proceeds.
    const DragPlane dp = drag_plane_for_node(*node, camera_forward);

    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const std::optional<simd_float3> hit = ray_plane(ray, dp.point, dp.normal);
    if (!hit) {
        return; // ray parallel to / behind the plane: drag does not activate
    }

    impl_->drag.plane_point = dp.point;
    impl_->drag.plane_normal = dp.normal;
    impl_->drag.start_pos = node->position;
    impl_->drag.start_hit = *hit;
    impl_->drag.start_snap_point = node->snap_point;
    impl_->drag.active = true;
}

void Editor::updateDrag(float x, float y) {
    if (!impl_->drag.active) {
        return;
    }
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return;
    }
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }

    const Camera camera = impl_->controller.to_camera();
    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const std::optional<simd_float3> hit = ray_plane(ray, impl_->drag.plane_point, impl_->drag.plane_normal);
    if (!hit) {
        return; // keep last position
    }

    const simd_float3 delta = *hit - impl_->drag.start_hit;
    node->position = impl_->drag.start_pos + delta;
    if (node->snapped) {
        // delta is in-plane (both hits lie on the same stored plane), so the
        // plane itself is unchanged by this update.
        node->snap_point = impl_->drag.start_snap_point + delta;
    }
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::endDrag() {
    impl_->drag.active = false;
}

Vec3f Editor::nodePosition(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }
    return Vec3f{node->position.x, node->position.y, node->position.z};
}

} // namespace sq
