#include <shapeshifter/ShapeshifterCore.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "camera.h"
#include "camera_controller.h"
#include "gizmo.h"
#include "navigation.h"
#include "picking.h"
#include "renderer.h"
#include "scene.h"

namespace sq {

// Camera-pivot marker sizing, screen-constant like the move gizmo: the ring's
// radius and line half-width in view points, converted to world units at the
// orbit target's depth.
inline constexpr float kPivotRadiusPts = 14.0f;
// Predictive pivot dot. Small on purpose: it redraws on every mouse move, so
// anything louder would be a permanent distraction rather than an affordance.
inline constexpr float kFocusPreviewRadiusPts = 3.0f;
inline constexpr float kPivotLineHalfWidthPts = 0.75f;

// World-origin marker sizing. The height is world-constant on purpose -- the
// +Y axis is a world landmark like the plate's X/Z lines, so it should shrink
// with distance rather than hold a fixed screen size the way a manipulator
// does. Only the shaft width and pip are screen-constant, to keep a steady
// visual weight. Geometry lives in lines.h (append_origin_marker); these are
// sizing policy, which is why they sit here alongside the pivot's.
inline constexpr float kOriginMarkerHeight = 3.0f;       // world units
inline constexpr float kOriginMarkerHalfWidthPts = 1.0f; // ~2pt shaft
inline constexpr float kOriginPipHalfSizePts = 2.5f;

struct Editor::Impl {
    Renderer renderer;
    SceneDocument scene;
    CameraController controller;
    float viewportWidthPts = 0.0f;  // for pick()'s ray_through_view_point
    float viewportHeightPts = 0.0f; // for the camera gestures, gizmo picking and pick()
    int32_t selected = kInvalidNode;

    bool gizmo_visible = false;

    // One drag gesture for both gizmos. Which fields are live depends on
    // slot/handle; all of them are frozen at beginDrag, so every update
    // re-derives from the press rather than integrating (the same
    // cumulative-from-start rule the camera gestures follow).
    struct {
        bool active = false;
        int32_t node_id = kInvalidNode; // the node the captured frame/start_* belong to
        GizmoSlot slot = GizmoSlot::Placement;
        GizmoHandle handle = GizmoHandle::None;
        GizmoFrame frame;               // frozen at beginDrag for the whole gesture
        float start_axis_s = 0.0f;      // axis handles: the axis parameter at mouse-down
        float start_y = 0.0f;           // uniform scale: press position, in view points
        simd_float3 start_pos, start_hit;
        simd_float3 start_scale;        // scale drags: the node's scale at mouse-down
        simd_float3 start_ring_dir{};   // ring handles: the grabbed direction, in the ring's plane
        simd_quatf start_rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);
    } drag;

    // Camera gesture. `start_controller` is the controller as it stood once the
    // gesture's re-pivot had been applied, so update() can restore it and
    // re-apply the whole cumulative delta from scratch — that is what makes
    // updateCameraGesture idempotent and immune to event coalescing.
    struct {
        bool active = false;
        CameraGesture kind = CameraGesture::Orbit;
        simd_float3 focus{};            // resolved ONCE, at begin
        float focus_depth = 0.0f;       // |focus - eye| at begin; pan's points-to-world scale
        CameraController start_controller;
        float last_dx = 0.0f, last_dy = 0.0f;
    } camera_gesture;

    // When the camera last moved (or a gesture began). The pivot marker is
    // derived from this rather than from endCameraGesture: a legacy scroll
    // wheel reports no gesture phases, so the app layer synthesises a
    // begin/update/end per event there and an end-driven marker would blink
    // off between events. A timestamp covers drag, phased trackpad gestures
    // and phase-less wheels uniformly.
    //
    // Initialised well in the past so the marker starts hidden (hold + fade is
    // 0.6s total). Not time_point::min(), whose distance from now() would
    // overflow the duration arithmetic below.
    std::chrono::steady_clock::time_point last_camera_activity =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);

    // Predictive pivot dot. Only ever set from a FocusSource::Scene hit, so
    // "valid" means "the cursor is over the model", not merely "resolved".
    bool focus_preview_valid = false;
    simd_float3 focus_preview{};

    // Hovered gizmo handle, and which of the two gizmos it belongs to (edit
    // mouse-moved feedback). Cleared anywhere the gizmo it points at can go
    // away: select(), gizmo hide, deleteSelectedNode — hover must never outlive
    // its gizmo.
    GizmoHit hover{GizmoSlot::Placement, GizmoHandle::None};

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
        const GizmoFrame placement =
            gizmo_frame_for_node(*selectedNode, camera, GizmoSlot::Placement);
        const GizmoFrame shape = gizmo_frame_for_node(*selectedNode, camera, GizmoSlot::Shape);
        // Mid-drag the active handle owns the highlight (mouseMoved doesn't
        // fire while the button is down, so hover would be stale anyway).
        const GizmoHit highlighted =
            impl_->drag.active ? GizmoHit{impl_->drag.slot, impl_->drag.handle} : impl_->hover;
        // The grid is the Placement gizmo's reference plane, and a reference is
        // only worth its screen area while you are moving something against it.
        const bool show_grid = impl_->drag.active && impl_->drag.slot == GizmoSlot::Placement;
        impl_->renderer.set_gizmos(placement, shape, highlighted, camera.eye, show_grid);
    } else {
        impl_->renderer.hide_gizmo();
    }

    // Screen-constant chrome. All of it needs the same conversion -- view
    // points to world units at some depth -- so the factor is derived once
    // here per subject depth. Everything below first appears once the initial
    // resize callback has landed and viewportHeightPts is real.
    if (impl_->viewportHeightPts > 0.0f) {
        const auto pts_to_world_at = [&](simd_float3 subject) {
            const float d = simd_length(subject - camera.eye);
            return 2.0f * d * std::tan(camera.fov_y_radians * 0.5f) / impl_->viewportHeightPts;
        };

        // Camera-pivot marker: visible only around a camera gesture. Alpha
        // reaches 0 shortly after the last camera move and set_pivot_marker
        // then emits nothing, so the resting cost is a subtraction.
        const float since_camera_move = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - impl_->last_camera_activity).count();
        // Drawn at the controller's PIVOT, not camera.target: since
        // cursor-anchored orbit those are different points, and the marker's
        // whole job is to show what a rotation will swing around.
        const simd_float3 pivot = impl_->controller.pivot();
        const float pivot_scale = pts_to_world_at(pivot);
        impl_->renderer.set_pivot_marker(pivot,
                                         kPivotRadiusPts * pivot_scale,
                                         kPivotLineHalfWidthPts * pivot_scale,
                                         camera.eye,
                                         pivot_marker_alpha(since_camera_move));

        // Predictive pivot dot, screen-constant like the pivot ring.
        if (impl_->focus_preview_valid) {
            impl_->renderer.set_focus_preview(impl_->focus_preview,
                                              kFocusPreviewRadiusPts *
                                                  pts_to_world_at(impl_->focus_preview),
                                              camera.eye);
        } else {
            impl_->renderer.hide_focus_preview();
        }

        // World-origin +Y axis and pip. Height is world-constant (it is a
        // world-space landmark, like the plate's X/Z axes) while the shaft
        // width and pip stay screen-constant, so the marker keeps a steady
        // visual weight without lying about how tall 3 units is.
        const float origin_scale = pts_to_world_at((simd_float3){0.0f, 0.0f, 0.0f});
        impl_->renderer.set_origin_marker(kOriginMarkerHeight,
                                          kOriginMarkerHalfWidthPts * origin_scale,
                                          kOriginPipHalfSizePts * origin_scale,
                                          camera.eye);
    }

    impl_->renderer.render(static_cast<CA::MetalDrawable*>(caMetalDrawable), impl_->scene, impl_->selected, camera);
}

void Editor::beginCameraGesture(CameraGesture kind, float anchorX, float anchorY) {
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return;
    }

    const Camera camera = impl_->controller.to_camera();
    const Ray ray = camera.ray_through_view_point(anchorX, anchorY, impl_->viewportWidthPts,
                                                  impl_->viewportHeightPts);
    const FocusPoint focus = resolve_focus(impl_->scene, ray, camera.target);

    // Orbit re-centres on what the press was aimed at. This does not move the
    // eye, so nothing on screen shifts and the scene line buffer (whose sphere
    // silhouettes depend only on eye position) stays valid — which is exactly
    // why re-deriving the pivot every gesture is affordable and invisible.
    if (kind == CameraGesture::Orbit) {
        impl_->controller.set_pivot_preserving_eye(focus.point);
    }

    impl_->camera_gesture.kind = kind;
    impl_->camera_gesture.focus = focus.point;
    impl_->camera_gesture.focus_depth = simd_length(focus.point - camera.eye);
    impl_->camera_gesture.start_controller = impl_->controller; // AFTER the re-pivot
    impl_->camera_gesture.last_dx = 0.0f;
    impl_->camera_gesture.last_dy = 0.0f;
    impl_->camera_gesture.active = true;

    // Light the pivot marker on press rather than on first movement, so the
    // thing you are about to rotate around is visible before you commit.
    impl_->last_camera_activity = std::chrono::steady_clock::now();
}

// Sphere outlines are view-dependent (silhouette from the current eye), so any
// camera move that actually happened invalidates the scene line buffer.
void Editor::updateCameraGesture(float dxTotal, float dyTotal) {
    if (!impl_->camera_gesture.active || impl_->viewportHeightPts <= 0.0f) {
        return;
    }
    if (!std::isfinite(dxTotal) || !std::isfinite(dyTotal)) {
        return;
    }
    if (dxTotal == impl_->camera_gesture.last_dx && dyTotal == impl_->camera_gesture.last_dy) {
        return; // nothing new to apply; re-deriving would be identical work
    }
    impl_->camera_gesture.last_dx = dxTotal;
    impl_->camera_gesture.last_dy = dyTotal;

    // Restore and re-apply the WHOLE delta rather than integrating the change
    // since the last event. Coalesced, duplicated or out-of-order updates then
    // all land on the same camera, and a gesture dragged back to its start
    // returns exactly there instead of accumulating rounding.
    impl_->controller = impl_->camera_gesture.start_controller;
    // Aspect is viewport state, not gesture state, and the controller happens
    // to hold both. Without this, a resize landing mid-gesture would be undone
    // by the very next update and never re-applied (no further resize event
    // follows), leaving a stretched projection and picking rays that disagree
    // with what is on screen. Reachable via a momentum scroll, which keeps a
    // gesture open for about a second after the fingers lift, or a move between
    // displays of different backing scale.
    impl_->controller.set_aspect(impl_->viewportWidthPts / impl_->viewportHeightPts);
    switch (impl_->camera_gesture.kind) {
        case CameraGesture::Orbit:
            impl_->controller.orbit(dxTotal, dyTotal);
            break;
        case CameraGesture::Pan:
            impl_->controller.pan_world(dxTotal, dyTotal, impl_->camera_gesture.focus_depth,
                                        impl_->viewportHeightPts);
            break;
        case CameraGesture::Dolly:
            impl_->controller.dolly_toward(
                impl_->camera_gesture.focus,
                std::exp(-dyTotal * CameraController::kDollySens));
            break;
    }

    impl_->last_camera_activity = std::chrono::steady_clock::now();
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::endCameraGesture() {
    impl_->camera_gesture.active = false;
}

float Editor::dollyPointsForMagnification(float cumulativeMagnification) const {
    return CameraController::dolly_points_for_magnification(cumulativeMagnification);
}

void Editor::frameSelected() {
    const Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }
    const Camera camera = impl_->controller.to_camera();
    const float radius = frame_radius_for_bound(node_bounding_radius(*node), camera.fov_y_radians);
    if (impl_->controller.frame_on(node->position, radius)) {
        impl_->last_camera_activity = std::chrono::steady_clock::now();
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
    impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None}; // the hovered handle belonged to the old selection's gizmo
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

    // An active drag must not survive the node it's driving going away.
    // Drags only ever target the currently selected node (beginDrag captures
    // state for impl_->selected alone), so ending unconditionally here is
    // equivalent to the old hand-inlined "if node_id == selected" reset, just
    // via the real endDrag() entry point instead of duplicating its body.
    // Order matters: end the gesture before remove_node, so nothing is left
    // referencing the id about to go away. Scale needs no separate call now
    // that both gizmo kinds share this one drag path.
    endDrag();

    impl_->scene.remove_node(impl_->selected);
    impl_->selected = kInvalidNode;
    impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None}; // deletion bypasses select(), so clear here too
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
        impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None}; // no gizmo, nothing to hover
    }
}

void Editor::updateGizmoHover(float x, float y) {
    // Same guards as beginDrag, plus gizmo visibility: an invisible gizmo has
    // no handles to hover. Failing any guard clears rather than keeps stale.
    impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None};
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f || !impl_->gizmo_visible) {
        return;
    }
    const Node* node = impl_->scene.find(impl_->selected);
    if (node == nullptr) {
        return;
    }

    const Camera camera = impl_->controller.to_camera();
    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    impl_->hover = pick_gizmos(gizmo_frame_for_node(*node, camera, GizmoSlot::Placement),
                               gizmo_frame_for_node(*node, camera, GizmoSlot::Shape), ray,
                               camera.fov_y_radians, impl_->viewportHeightPts);
}

void Editor::updateFocusPreview(float x, float y) {
    impl_->focus_preview_valid = false;
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f) {
        return;
    }
    const Camera camera = impl_->controller.to_camera();
    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts,
                                                   impl_->viewportHeightPts);
    const FocusPoint focus = resolve_focus(impl_->scene, ray, camera.target);
    if (focus.source != FocusSource::Scene) {
        return; // fallbacks are real focus points but not things worth pointing at
    }
    impl_->focus_preview = focus.point;
    impl_->focus_preview_valid = true;
}

void Editor::clearFocusPreview() {
    impl_->focus_preview_valid = false;
}

void Editor::clearGizmoHover() {
    impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None};
}

GizmoHit Editor::gizmoHoverHandle() const {
    return impl_->hover;
}

bool Editor::beginDrag(float x, float y) {
    // Same guards as updateGizmoHover, gizmo visibility included: an
    // invisible gizmo has no handles to hover AND none to grab. The two used
    // to disagree, which meant a click at a hidden handle's last position
    // still started a real drag and moved the node (branch review finding,
    // pinned by drag_tests).
    if (impl_->viewportWidthPts <= 0.0f || impl_->viewportHeightPts <= 0.0f ||
        !impl_->gizmo_visible) {
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
    const GizmoFrame placement = gizmo_frame_for_node(*node, camera, GizmoSlot::Placement);
    const GizmoFrame shape = gizmo_frame_for_node(*node, camera, GizmoSlot::Shape);

    const Ray ray = camera.ray_through_view_point(x, y, impl_->viewportWidthPts, impl_->viewportHeightPts);
    const GizmoHit hit =
        pick_gizmos(placement, shape, ray, camera.fov_y_radians, impl_->viewportHeightPts);
    if (hit.handle == GizmoHandle::None) {
        // Off-handle. Returning false is what lets the app layer hand the
        // press to the camera instead — the seam the always-on camera model
        // is built on.
        return false;
    }
    const GizmoSlot slot = hit.slot;
    const GizmoHandle handle = hit.handle;
    // Which manipulator you get is decided by the handle you grabbed, so the
    // frame the whole gesture works in follows from that, not from any mode.
    const GizmoFrame frame = (slot == GizmoSlot::Shape) ? shape : placement;

    if (slot == GizmoSlot::Shape) {
        if (gizmo_handle_is_axis(handle)) {
            // Floored on capture AND on every update, so the ratio starts at
            // exactly 1 and can never divide by ~0 or flip sign (gizmo.h).
            const std::optional<float> s = scale_axis_param(ray, frame, handle);
            if (!s) {
                return false; // near-parallel axis: the solver has nothing to offer
            }
            impl_->drag.start_axis_s = *s;
        }
        // The uniform handle needs no ray state: it is a screen-space vertical
        // drag, captured via start_y below.
        impl_->drag.start_scale = node->scale;
    } else if (gizmo_handle_is_ring(handle)) {
        const std::optional<simd_float3> dir = ring_drag_dir(ray, frame, handle);
        if (!dir) {
            return false; // edge-on or degenerate: the solver has no angle to offer
        }
        impl_->drag.start_ring_dir = *dir;
        impl_->drag.start_rotation = node->rotation;
    } else if (gizmo_handle_is_axis(handle)) {
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
    impl_->drag.slot = slot;
    impl_->drag.handle = handle;
    impl_->drag.start_y = y;
    impl_->drag.start_pos = node->position;
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

    if (impl_->drag.slot == GizmoSlot::Shape) {
        // Cumulative from the press, never incremental: both branches derive
        // the result from start_scale and the total travel, so the outcome
        // depends only on where the cursor is now.
        simd_float3 scale = impl_->drag.start_scale;
        if (impl_->drag.handle == GizmoHandle::Uniform) {
            scale *= std::exp(-(y - impl_->drag.start_y) * kUniformScaleSens);
        } else {
            const std::optional<float> s = scale_axis_param(ray, impl_->drag.frame, impl_->drag.handle);
            if (!s) {
                return; // near-parallel: keep the last scale
            }
            const int axis = gizmo_scale_axis_index(impl_->drag.handle);
            // start_axis_s carries the same floor, so this ratio is positive
            // and finite by construction — see scale_axis_param.
            scale[axis] = impl_->drag.start_scale[axis] * (*s / impl_->drag.start_axis_s);
        }
        node->scale = simd_float3{
            std::clamp(scale.x, kNodeScaleMin, kNodeScaleMax),
            std::clamp(scale.y, kNodeScaleMin, kNodeScaleMax),
            std::clamp(scale.z, kNodeScaleMin, kNodeScaleMax),
        };
        impl_->renderer.set_scene_lines_dirty();
        return;
    }

    if (gizmo_handle_is_ring(impl_->drag.handle)) {
        const std::optional<simd_float3> dir =
            ring_drag_dir(ray, impl_->drag.frame, impl_->drag.handle);
        if (!dir) {
            return; // edge-on or degenerate: keep the last rotation
        }
        const simd_float3 axis = gizmo_ring_axis(impl_->drag.frame, impl_->drag.handle);
        // Cumulative from the press, like every other gesture here: the angle
        // is measured from the direction grabbed at mouse-down, so the result
        // depends only on where the cursor is NOW. No winding state, which
        // caps one gesture at half a turn -- past +-pi the shortest-path
        // reading reverses, and a second drag continues the spin. That is the
        // price of an angle that cannot mis-count a winding on a fast flick.
        const float theta = signed_angle_about(impl_->drag.start_ring_dir, *dir, axis);
        const simd_quatf q = simd_quaternion(theta, axis);
        // Renormalised because pack_scene conjugates rather than inverts, which
        // is only the same thing for a unit quaternion (see sdf.cpp). This is
        // the one place rotation is produced, so it is the one place that has
        // to hold up the precondition.
        node->rotation = simd_normalize(simd_mul(q, impl_->drag.start_rotation));
        // Swing the node AROUND the anchor rather than spinning it in place:
        // for an attached detail the anchor is its contact point, so this is
        // what keeps it touching the surface. A free node's anchor is its own
        // centre, making the second term zero -- one formula, both cases.
        node->position = impl_->drag.frame.origin +
                         simd_act(q, impl_->drag.start_pos - impl_->drag.frame.origin);
        // snap_point and snap_normal are deliberately untouched: the surface
        // did not move, and the contact point is what we just rotated about.
        impl_->renderer.set_scene_lines_dirty();
        return;
    }

    simd_float3 delta;
    if (gizmo_handle_is_axis(impl_->drag.handle)) {
        const simd_float3 axis = gizmo_axis_dir(impl_->drag.frame, impl_->drag.handle);
        // Same view-align guard as pick_gizmo_handle, here per-update: an
        // axis grabbed just inside the pick limit can still be dragged
        // toward its vanishing point, where the solver's 1/sin^2 term
        // amplifies cursor noise into huge jumps. Keep the last position
        // instead (post-R3 review finding).
        if (std::fabs(simd_dot(ray.dir, axis)) > kAxisViewAlignLimit) {
            return;
        }
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
    // snap_point deliberately does NOT follow. The attachment point is a fact
    // about the surface the detail was placed on, not about where the detail
    // has since been dragged to -- so pulling a node along its normal leaves
    // the attachment on the skin and opens a visible offset between the two
    // gizmos, which is exactly what the tether reports.
    //
    // This reverses the move-gizmo spec's rigid-ride ruling, and it has to:
    // with the node spawning centred ON its snap point, a snap frame that rode
    // along would keep the two anchors equal forever and the Placement/Shape
    // split could never be observed at all.
    impl_->renderer.set_scene_lines_dirty();
}

void Editor::endDrag() {
    impl_->drag.active = false;
    impl_->drag.node_id = kInvalidNode;
    // The drag may have moved the gizmo out from under the cursor, so the
    // pre-drag hover is stale. The app layer re-derives hover from the
    // mouse-up position right after this (EditorViewModel.handleMouseUp);
    // for positionless callers (mode-switch aborts, deletion) None is right.
    impl_->hover = GizmoHit{GizmoSlot::Placement, GizmoHandle::None};
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

Vec3f Editor::nodeScale(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }
    return Vec3f{node->scale.x, node->scale.y, node->scale.z};
}

Vec4f Editor::nodeRotation(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return Vec4f{0.0f, 0.0f, 0.0f, 1.0f}; // identity, not zero: a zero quaternion is not a rotation
    }
    const simd_float4 q = node->rotation.vector;
    return Vec4f{q.x, q.y, q.z, q.w};
}

Shape Editor::nodeShape(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    return node != nullptr ? node->shape : Shape::Cube;
}

float Editor::nodeShapeParam(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    return node != nullptr ? node->shape_param : 0.0f;
}

ShapeParamSpec Editor::nodeShapeParamSpec(int32_t nodeId) const {
    const Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return ShapeParamSpec{false, 0.0f, 0.0f, 0.0f, 0.0f, false};
    }
    return shape_param_spec(node->shape);
}

void Editor::setNodeShapeParam(int32_t nodeId, float value) {
    Node* node = impl_->scene.find(nodeId);
    if (node == nullptr) {
        return;
    }
    // Clamped and snapped HERE rather than at the call site, so the app layer
    // can hand over a raw cursor-derived angle and no caller can put a node
    // into a state the evaluator has to defend against. A paramless shape
    // snaps to 0, which is what its evaluator ignores anyway.
    node->shape_param = snap_shape_param(shape_param_spec(node->shape), value);
    // Unlike setNodeOp's, this one is load-bearing: the wireframe builders read
    // shape_param (a cone's slant lines and a prism's side count both move with
    // it), so a stale line buffer would show the previous value.
    impl_->renderer.set_scene_lines_dirty();
}

} // namespace sq
