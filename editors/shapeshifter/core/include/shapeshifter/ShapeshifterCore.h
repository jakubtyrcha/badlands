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
// Ids must match the SDF_SHAPE_* defines in shaders/sdf_scene.h, which is the
// MSL-side copy (that header compiles as Metal and cannot include this one).
// core/src/sdf.cpp static_asserts every pair, so the two cannot drift.
enum class Shape : int32_t {
    Cube = 0, Sphere = 1, Cone = 2, Capsule = 3,
    Octahedron = 4, Pyramid = 5, Prism = 6, Vesica = 7
};
inline constexpr int32_t kShapeCount = 8;
enum class Op    : int32_t { Add = 0, Subtract = 1 };

// The one profile parameter a shape may carry -- the degree of freedom its
// bounding box cannot state (how blunt a cone's tip is, how round a capsule's
// caps are, how many faces a prism has). Shapes whose proportions are fully
// determined by the box report has_param == false; for them every other field
// is zero and there is no dial to draw.
//
// SINGLE SOURCE OF TRUTH, and deliberately so: core clamps and snaps against
// these numbers in setNodeShapeParam, and the app layer reads the same struct
// for the dial's ends, its detents and its readout format. Spelling the range
// out independently on both sides of the boundary is exactly the drift the
// gizmo's shared kGizmoPatch* constants exist to prevent.
struct ShapeParamSpec {
    bool  has_param;
    float min_value;
    float max_value;
    float step;           // snap granularity: 0.05 continuous, 1 integral
    float default_value;  // what a freshly spawned node of this shape gets
    bool  integral;       // formatting hint: show "6", not "6.00"
};

// The table itself (core/src/scene.cpp). A pure function of Shape, so it sits
// beside the type rather than behind Editor; Editor::nodeShapeParamSpec is the
// per-node convenience the app layer actually calls.
ShapeParamSpec shape_param_spec(Shape shape);

// Clamps to [min_value, max_value] and snaps to the nearest multiple of `step`.
// A paramless shape's spec snaps everything to 0. Shared so core's setter and
// any caller that wants to predict the result cannot disagree about rounding.
float snap_shape_param(const ShapeParamSpec& spec, float value);

// Gizmo handles. Crosses the boundary like Shape/Op: the canonical definition
// lives here, core/src/gizmo.h consumes it. Axis order is also the pick
// tie-break order (see pick_gizmo_handle). Placement uses the axes, the plane
// and the rings; Shape uses the axes plus Uniform. The names are shared between
// the two slots on purpose -- AxisU means "the first axis of whichever gizmo
// this is" -- so a handle is only meaningful alongside its GizmoSlot, which is
// why picking returns the pair (see GizmoHit).
enum class GizmoHandle : int32_t {
    None = 0, AxisU = 1, AxisV = 2, AxisN = 3,
    // The ONE two-axis drag handle, in the Placement gizmo's grid plane -- the
    // tangent plane for an attached node, world-horizontal for a free one. There
    // were three (one per basis pair) until they proved to be the widest, most
    // occluding thing on a coalesced gizmo for the least-used gesture; the
    // surviving one is the tangent drag the whole attachment model is about.
    Plane = 4,
    Uniform = 5,
    // Rotation rings, one about each of the frame's axes. Placement only: a
    // detail swivels against the surface it sits on, which is a fact about
    // where it is, not about how big it is.
    RingU = 6, RingV = 7, RingN = 8
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
    // No drawable: the RHI's swapchain acquires one itself. The app drives this
    // from a plain CADisplayLink for that reason -- CAMetalDisplayLink vends a
    // drawable on every callback, and two consumers of one drawable pool empty
    // it and stop the frame.
    void render();

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

    // deletes the selected node; no-op without a selection; clears selection
    // and refreshes line colors. Undoable like every other edit, provided the
    // caller brackets it in an interaction (see below).
    void deleteSelectedNode();

    // --- interactions and history ------------------------------------------
    //
    // An INTERACTION is a user gesture, and the unit of undo. THE APP DECLARES
    // EVERY BOUNDARY, uniformly -- a drag, a dial turn, a click that spawns, a
    // key that deletes -- because the app is what owns gestures. Core decides
    // none of them, and beginDrag/endDrag are about GIZMO STATE only; they
    // touch history not at all.
    //
    // Everything mutated between the outermost begin and end becomes ONE undo
    // entry. The intermediate states a drag writes are temporary and are never
    // recorded: at endInteraction the live document is decomposed against the
    // baseline captured at begin, so what lands is the gesture's net result
    // rather than the path the cursor took.
    //
    // Refcounted, so a nested pair cannot split one gesture in two. An
    // interaction that changes nothing leaves no entry, which is what makes a
    // click that merely selects, and a dial press that turns nothing, free.
    //
    // `label` is what the Edit menu shows ("Move", "Delete"). Not copied beyond
    // the call.
    void beginInteraction(const char* label);
    void endInteraction();

    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    // NUL-terminated fill, "" when there is nothing to undo/redo -- the same
    // contract nodeName follows.
    void undoLabel(char* buf, int32_t bufLen) const;
    void redoLabel(char* buf, int32_t bufLen) const;

    // Which handle the running drag grabbed, so the app can name the
    // interaction "Move" / "Rotate" / "Scale" without guessing. `.handle` is
    // None when no drag is active.
    //
    // Safe to call right after a successful beginDrag: that call captures gizmo
    // state and mutates the document not at all, so opening the interaction
    // AFTER it still brackets every edit the gesture makes.
    GizmoHit activeDragHandle() const;

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

    // --- shape parameter (the radial menu's arc dial) ----------------------
    //
    // The dial's whole contract lives here: the spec says what the range is,
    // the setter is the only thing that writes the value, and it clamps and
    // snaps on the way in — so the app layer can send raw cursor-derived
    // values and cannot put a node into a state the renderer has to defend
    // against.
    Shape nodeShape(int32_t nodeId) const;               // Shape::Cube for unknown id
    float nodeShapeParam(int32_t nodeId) const;          // 0 for unknown id
    void setNodeShapeParam(int32_t nodeId, float value); // clamps, snaps, dirties the lines
    // has_param == false for an unknown id as well as for a paramless shape:
    // either way there is no dial to draw, which is the only question the
    // caller is asking.
    ShapeParamSpec nodeShapeParamSpec(int32_t nodeId) const;

private:
    Editor();
    struct Impl;
    Impl* impl_;
};

} // namespace sq
