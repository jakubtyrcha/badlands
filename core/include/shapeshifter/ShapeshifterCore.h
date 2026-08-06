#pragma once
#include <cstdint>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace sq {

struct Vec3f { float x, y, z; };
// A unit quaternion, xyz imaginary and w real — the same component order
// simd_quatf::vector uses, so crossing the boundary is a field copy.
struct Vec4f { float x, y, z, w; };

// Shape/Op cross the interop boundary (spawn() below, and Node::shape/op in
// core/src/scene.h), so their canonical definitions live here rather than in
// a core/src-private header. scene.h includes this header instead of
// redefining them.
enum class Shape : int32_t { Cube = 0, Sphere = 1 };
enum class Op    : int32_t { Add = 0, Subtract = 1 };

// Gizmo handles. Crosses the boundary like Shape/Op: the canonical definition
// lives here, core/src/gizmo.h consumes it. Axis order is also the pick
// tie-break order (see pick_gizmo_handle). Placement uses the axis and plane
// handles; Shape uses the axes plus Uniform, and no planes. The names are
// shared between the two slots on purpose -- AxisU means "the first axis of
// whichever gizmo this is" -- so a handle is only meaningful alongside its
// GizmoSlot, which is why picking returns the pair (see GizmoHit).
enum class GizmoHandle : int32_t {
    None = 0, AxisU = 1, AxisV = 2, AxisN = 3, PlaneUV = 4, PlaneUN = 5, PlaneVN = 6,
    Uniform = 7,
    // Rotation rings, one about each of the frame's axes. Placement only: a
    // detail swivels against the surface it sits on, which is a fact about
    // where it is, not about how big it is.
    RingU = 8, RingV = 9, RingN = 10
};

// Which of the two manipulators a handle belongs to. A selected node shows
// BOTH at once; they are not modes and there is nothing to arm.
//
// Placement answers "where does this node sit" and is anchored at the node's
// attachment point -- its snap point when it has one, its own centre when it
// does not -- in the surface's tangent frame. Shape answers "how big is this
// node" and is always anchored at the node's centre in the node's own local
// axes, because a scale handle has to map onto a scale COMPONENT.
//
// For a node that is not attached, or one whose centre still sits on its snap
// point, the two anchors coincide and the pair reads as a single gizmo. That
// is emergent rather than a special case: with the same origin, drawing both
// handle sets IS the combined gizmo, and their radius bands are disjoint so
// nothing collides.
enum class GizmoSlot : int32_t { Placement = 0, Shape = 1 };

// A hit on one of the two gizmos. `handle` is None exactly when nothing was
// hit, in which case `slot` carries no meaning.
struct GizmoHit { GizmoSlot slot; GizmoHandle handle; };

// Camera gesture verbs. Each has exactly one trackpad gesture and one pointer
// chord in the app layer; nothing is bound twice.
enum class CameraGesture : int32_t { Orbit = 0, Pan = 1, Dolly = 2 };

// Miss/no-selection/no-parent sentinel for the picking, selection, and
// spawning APIs below, and for SceneDocument's own Node::id/snap_parent
// (core/src/scene.h). Previously left undeclared here (see M4's deferred
// finding) because a second same-named `inline constexpr` in scene.h would
// ODR-conflict with any TU including both headers; now that scene.h includes
// this header instead of defining its own copy, a single definition works.
inline constexpr int32_t kInvalidNode = -1;

struct PickResult {
    int32_t node_id;   // kInvalidNode on miss
    Vec3f point;
    Vec3f normal;
};

struct SpawnResult { int32_t node_id; bool snapped; };

struct ScreenPoint { float x, y; bool visible; };   // view points, top-left origin

// One app-lifetime instance; Swift imports this as a reference type.
class SWIFT_IMMORTAL_REFERENCE Editor {
public:
    static Editor* create();

    // Renderer lifecycle. Pointers are Obj-C objects passed as opaque void*,
    // borrowed — core never releases them.
    void attachLayer(void* caMetalLayer);
    void setViewportSize(float widthPts, float heightPts, float backingScale);
    void render(void* caMetalDrawable);

    // --- camera gestures ---------------------------------------------------
    //
    // Every gesture is begin/update/end. `begin` resolves the world point
    // under (anchorX, anchorY) ONCE — the model, else the ground plate, else
    // the target's depth — and everything the gesture does is expressed
    // against that one point, so the frame of reference cannot shift under the
    // user mid-drag. Orbit additionally re-centres on it, which never moves the
    // eye (see CameraController::set_pivot_preserving_eye).
    //
    // The anchor is the mouse-down point for drags and the cursor position for
    // scroll/pinch. Passing the mouse-down point rather than the current one is
    // what makes "point at the feature and drag" work.
    void beginCameraGesture(CameraGesture kind, float anchorX, float anchorY);

    // CUMULATIVE displacement from the gesture's start, in view points — the
    // same convention the drag/scale gestures use. Each call re-derives the
    // camera from the state captured at `begin` rather than integrating, so
    // the result depends only on the current total: repeated or coalesced
    // events cannot drift, and a delta that returns to zero returns the camera
    // exactly to where the gesture started. Dolly reads dyTotal only.
    void updateCameraGesture(float dxTotal, float dyTotal);
    void endCameraGesture();

    // Points of dolly drag equivalent to a cumulative pinch magnification, so
    // a pinch and the matching drag zoom identically and no sensitivity
    // constant has to be duplicated in the app layer.
    float dollyPointsForMagnification(float cumulativeMagnification) const;

    // Re-centres the camera on the selected node and ranges to fit it, WITHOUT
    // reorienting — framing answers "where", not "from where". No-op without a
    // selection. The fitted radius has a floor, so framing a very small node
    // leaves surrounding context rather than filling the view with
    // featureless surface.
    void frameSelected();

    // selection / picking (coords: view points, top-left origin)
    PickResult pick(float x, float y) const;   // pure query; does NOT change selection
    void select(int32_t nodeId);               // -1 (kInvalidNode) clears; refreshes line colors
    int32_t selectedNode() const;
    void nodeName(int32_t nodeId, char* buf, int32_t bufLen) const;  // NUL-terminated fill; "" if unknown id

    // spawning (creates node, selects it, refreshes line colors)
    SpawnResult spawn(Shape shape, Op op, float x, float y);   // view points, top-left origin

    // deletes the selected node (permanent, no undo); no-op without a
    // selection; clears selection and refreshes line colors
    void deleteSelectedNode();

    // Manipulators — core owns all gizmo math. beginDrag hit-tests BOTH gizmos
    // and returns whether a drag activated; off-handle presses return false,
    // which is what lets the app layer hand the gesture to the camera instead.
    // One drag path serves both slots: Placement solves an axis/plane, Shape
    // solves an axis ratio or a screen-space vertical drag for the uniform
    // handle, and core captures the press position so the
    // cumulative-from-start arithmetic lives in one place.
    //
    // There is no tool to arm: both gizmos are live whenever one is visible, so
    // which manipulator you get is decided by which handle you grab.
    void setGizmoVisible(bool visible);
    bool beginDrag(float x, float y);
    void updateDrag(float x, float y);
    void endDrag();

    // Predictive pivot dot: the surface point under the cursor — what the next
    // orbit would rotate around. Answers "how will this drag behave?" BEFORE
    // the press, which the pivot marker cannot: that only appears once a
    // gesture is already running. Lights ONLY on a real surface hit; a ground
    // or target-plane fallback draws nothing, because a dot floating in space
    // is noise rather than information.
    void updateFocusPreview(float x, float y);
    void clearFocusPreview();

    // Gizmo hover (modify-mode mouse-moved feedback). Hover state never
    // outlives the gizmo it points at: it self-clears on selection change,
    // gizmo hide, and node deletion — the app layer's clears are only a
    // belt to these suspenders.
    void updateGizmoHover(float x, float y);
    void clearGizmoHover();
    GizmoHit gizmoHoverHandle() const;   // .handle == None when nothing is hovered

    // node info (tests + later UI)
    Vec3f nodePosition(int32_t nodeId) const;   // {0,0,0} for unknown id

    // radial-menu anchor: the selected node's position projected to the viewport.
    // {0,0,false} when no selection, zero viewport, or the point is behind the camera.
    ScreenPoint projectSelectedAnchor() const;

    // op (menu toggle + color coding)
    Op nodeOp(int32_t nodeId) const;                // Op::Add for unknown id (documented)
    void setNodeOp(int32_t nodeId, Op op);          // marks scene lines dirty

    // node info (tests)
    Vec3f nodeScale(int32_t nodeId) const;          // {0,0,0} for unknown id
    Vec4f nodeRotation(int32_t nodeId) const;       // identity {0,0,0,1} for unknown id

private:
    Editor();
    struct Impl;
    Impl* impl_;
};

} // namespace sq
