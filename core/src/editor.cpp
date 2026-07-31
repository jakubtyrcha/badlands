#include <shapeshifter/ShapeshifterCore.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "camera.h"
#include "camera_controller.h"
#include "gizmo.h"
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
        int32_t node_id = kInvalidNode; // the node the captured frame/start_* belong to
        GizmoHandle handle = GizmoHandle::None;
        GizmoFrame frame;               // frozen at beginDrag for the whole gesture
        float start_axis_s = 0.0f;      // axis handles: ray_axis_param at mouse-down
        simd_float3 start_pos, start_hit, start_snap_point;
    } drag;

    // Hovered gizmo handle (modify-mode mouse-moved feedback). Cleared
    // anywhere the gizmo it points at can go away: select(), gizmo hide,
    // deleteSelectedNode — hover must never outlive its gizmo.
    GizmoHandle hover = GizmoHandle::None;

    // Mirrors drag's shape, including the node_id mid-gesture guard: without
    // it, a selection change between beginScale/updateScale (no interleaving
    // endScale) would silently apply the old node's start_scale to whatever
    // is selected now, the same class of bug the M6 drag fix addressed.
    struct {
        bool active = false;
        int32_t node_id = kInvalidNode; // the node start_scale was captured for
        simd_float3 start_scale;
    } scale_drag;
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
    if (widthPts > 0.0f && heightPts > 0.0f) {
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
        const GizmoFrame frame = gizmo_frame_for_node(*selectedNode, camera);
        impl_->renderer.set_gizmo(frame.origin, frame.n, frame.half_extent);
    } else {
        impl_->renderer.hide_gizmo();
    }

    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable), impl_->scene, impl_->selected, camera);
}

// Sphere outlines are view-dependent (silhouette from the current eye), so any
// camera move that actually happened invalidates the scene line buffer.
void Editor::cameraOrbit(float dxPts, float dyPts) {
    if (impl_->controller.orbit(dxPts, dyPts)) {
        impl_->renderer.set_scene_lines_dirty();
    }
}

void Editor::cameraZoom(float delta) {
    if (impl_->controller.zoom(delta)) {
        impl_->renderer.set_scene_lines_dirty();
    }
}

void Editor::cameraPan(float dxPts, float dyPts) {
    if (impl_->viewportHeightPts > 0.0f && impl_->controller.pan_view(dxPts, dyPts, impl_->viewportHeightPts)) {
        impl_->renderer.set_scene_lines_dirty();
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
    impl_->hover = GizmoHandle::None; // the hovered handle belonged to the old selection's gizmo
    impl_->renderer.set_scene_lines_dirty(); // selection change alters which node's wireframe (if any) is drawn
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

void Editor::deleteSelectedNode() {
    if (impl_->selected == kInvalidNode) {
        return; // no-op without a selection
    }

    // An active drag/scale gesture must not survive the node it's driving
    // going away — mirrors setMode's mid-gesture abort. Gestures only ever
    // target the currently selected node (beginDrag/beginScale only capture
    // state for impl_->selected), so ending both unconditionally here is
    // equivalent to the old hand-inlined "if node_id == selected" reset,
    // just via the real endDrag()/endScale() entry points instead of
    // duplicating their body. Order matters: end the gestures before
    // remove_node, so nothing is left referencing the id about to go away.
    endDrag();
    endScale();

    impl_->scene.remove_node(impl_->selected);
    impl_->selected = kInvalidNode;
    impl_->hover = GizmoHandle::None; // deletion bypasses select(), so clear here too
    // Gizmo hides on its own next render(): selectedNode is looked up via
    // impl_->scene.find(impl_->selected), which is null once selected is
    // kInvalidNode, regardless of gizmo_visible — no separate flag to clear.
    impl_->renderer.set_scene_lines_dirty();
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
    if (!visible) {
        impl_->hover = GizmoHandle::None; // no gizmo, nothing to hover
    }
}

void Editor::updateGizmoHover(float x, float y) {
    // Same guards as beginDrag, plus gizmo visibility: an invisible gizmo has
    // no handles to hover. Failing any guard clears rather than keeps stale.
    impl_->hover = GizmoHandle::None;
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f || !impl_->gizmo_visible) {
        return;
    }
    const Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }

    const Camera camera = impl_->controller.to_camera();
    const GizmoFrame frame = gizmo_frame_for_node(*node, camera);
    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    impl_->hover = pick_gizmo_handle(frame, ray, camera.fov_y_radians, impl_->viewportHeightPts);
}

void Editor::clearGizmoHover() {
    impl_->hover = GizmoHandle::None;
}

GizmoHandle Editor::gizmoHoverHandle() const {
    return impl_->hover;
}

bool Editor::beginDrag(float x, float y) {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return false;
    }
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return false; // no valid selection: drag does not activate
    }

    const Camera camera = impl_->controller.to_camera();
    // Captured NOW: the frame (basis AND half_extent) is fixed for the whole
    // drag, even though the node (and, for a snapped node, its snap fields)
    // moves as the drag proceeds.
    const GizmoFrame frame = gizmo_frame_for_node(*node, camera);

    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const GizmoHandle handle = pick_gizmo_handle(frame, ray, camera.fov_y_radians, impl_->viewportHeightPts);
    if (handle == GizmoHandle::None) {
        return false; // off-handle: moving is deliberate, via handles only (user ruling)
    }

    if (gizmo_handle_is_axis(handle)) {
        const std::optional<float> s = ray_axis_param(ray, frame.origin, gizmo_axis_dir(frame, handle));
        if (!s) {
            return false; // grabbed a handle the solver can't parameterize: don't activate
        }
        impl_->drag.start_axis_s = *s;
    } else {
        const std::optional<simd_float3> hit = ray_plane(ray, frame.origin, gizmo_plane_normal(frame, handle));
        if (!hit) {
            return false; // ray parallel to / behind the plane: drag does not activate
        }
        impl_->drag.start_hit = *hit;
    }

    impl_->drag.frame = frame;
    impl_->drag.handle = handle;
    impl_->drag.start_pos = node->position;
    impl_->drag.start_snap_point = node->snap_point;
    impl_->drag.node_id = node->id;
    impl_->drag.active = true;
    return true;
}

void Editor::updateDrag(float x, float y) {
    if (!impl_->drag.active) {
        return;
    }
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return;
    }
    // Defense in depth: the captured plane/start_* state belongs to the node
    // that was selected when beginDrag ran. If the selection has since
    // changed (e.g. a caller drives updateDrag without a matching
    // begin/endDrag pair around every selection change), applying that stale
    // state to whatever is selected now would silently jump the wrong node.
    if (impl_->selected != impl_->drag.node_id) {
        return;
    }
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }

    const Camera camera = impl_->controller.to_camera();
    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);

    simd_float3 delta;
    if (gizmo_handle_is_axis(impl_->drag.handle)) {
        const simd_float3 axis = gizmo_axis_dir(impl_->drag.frame, impl_->drag.handle);
        const std::optional<float> s = ray_axis_param(ray, impl_->drag.frame.origin, axis);
        if (!s) {
            return; // near-parallel: keep last position (mirrors ray_plane's guard)
        }
        delta = (*s - impl_->drag.start_axis_s) * axis;
    } else {
        const std::optional<simd_float3> hit =
            ray_plane(ray, impl_->drag.frame.origin, gizmo_plane_normal(impl_->drag.frame, impl_->drag.handle));
        if (!hit) {
            return; // keep last position
        }
        delta = *hit - impl_->drag.start_hit;
    }

    node->position = impl_->drag.start_pos + delta;
    if (node->snapped) {
        // Full delta, not just the in-plane part: the remembered snap frame
        // rides rigidly with the node, so an axis-n pull lifts it off its
        // surface without re-basing the frame mid-gesture (move-gizmo spec).
        node->snap_point = impl_->drag.start_snap_point + delta;
    }
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::endDrag() {
    impl_->drag.active = false;
    impl_->drag.node_id = kInvalidNode;
}

Vec3f Editor::nodePosition(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }
    return Vec3f{node->position.x, node->position.y, node->position.z};
}

ScreenPoint Editor::projectSelectedAnchor() const {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return ScreenPoint{0.0f, 0.0f, false};
    }
    const Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return ScreenPoint{0.0f, 0.0f, false};
    }

    // ViewPoint already returns {0,0,false} when the point is behind the
    // camera (clip.w <= 0), so this is a direct field-for-field mapping.
    const ViewPoint vp = impl_->controller.to_camera().project(
        node->position, impl_->viewportWidthPts, impl_->viewportHeightPts);
    return ScreenPoint{vp.x, vp.y, vp.visible};
}

Op Editor::nodeOp(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    return node != nullptr ? node->op : Op::Add;
}

void Editor::setNodeOp(int32_t nodeId, Op op) {
    Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return;
    }
    node->op = op;
    // The wireframe no longer varies with op (always kColorSelected), so this
    // dirty call is not strictly required by the current draw -- kept anyway
    // because it's cheap, op changes are rare, and it keeps the invalidation
    // rule simple ("any node edit dirties the lines") rather than requiring
    // every call site to reason about which fields the wireframe reads.
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::beginScale() {
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return; // no valid selection: scale does not activate
    }
    impl_->scale_drag.start_scale = node->scale;
    impl_->scale_drag.node_id = node->id;
    impl_->scale_drag.active = true;
}

void Editor::updateScale(float pixelDeltaY) {
    if (!impl_->scale_drag.active) {
        return;
    }
    // Defense in depth, mirroring updateDrag: the captured start_scale
    // belongs to the node that was selected when beginScale ran. If the
    // selection has since changed with no interleaving endScale, applying
    // that stale start_scale to whatever is selected now would silently
    // rescale the wrong node.
    if (impl_->selected != impl_->scale_drag.node_id) {
        return;
    }
    Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }

    // Cumulative from the captured start scale, not incremental from the
    // node's current scale: pixelDeltaY is always the total delta from the
    // drag's start, so re-deriving from start_scale every call keeps the
    // result independent of how many updateScale calls happened in between.
    const float factor = std::exp(-pixelDeltaY * 0.005f);
    const simd_float3& start = impl_->scale_drag.start_scale;
    node->scale = simd_float3{
        std::clamp(start.x * factor, 0.05f, 50.0f),
        std::clamp(start.y * factor, 0.05f, 50.0f),
        std::clamp(start.z * factor, 0.05f, 50.0f),
    };
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::endScale() {
    impl_->scale_drag.active = false;
    impl_->scale_drag.node_id = kInvalidNode;
}

Vec3f Editor::nodeScale(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }
    return Vec3f{node->scale.x, node->scale.y, node->scale.z};
}

} // namespace sq
