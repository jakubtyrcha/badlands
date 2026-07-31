#pragma once
#include <cstdint>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace sq {

struct Vec3f { float x, y, z; };

// Shape/Op cross the interop boundary (spawn() below, and Node::shape/op in
// core/src/scene.h), so their canonical definitions live here rather than in
// a core/src-private header. scene.h includes this header instead of
// redefining them.
enum class Shape : int32_t { Cube = 0, Sphere = 1 };
enum class Op    : int32_t { Add = 0, Subtract = 1 };

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

    // camera tool (deltas in view points; delta semantics match CameraController)
    void cameraOrbit(float dxPts, float dyPts);
    void cameraZoom(float delta);
    void cameraPan(float dxPts, float dyPts);

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

    // modify tool — core owns all plane math
    void setGizmoVisible(bool visible);
    void beginDrag(float x, float y);
    void updateDrag(float x, float y);
    void endDrag();

    // node info (tests + later UI)
    Vec3f nodePosition(int32_t nodeId) const;   // {0,0,0} for unknown id

    // radial-menu anchor: the selected node's position projected to the viewport.
    // {0,0,false} when no selection, zero viewport, or the point is behind the camera.
    ScreenPoint projectSelectedAnchor() const;

    // op (menu toggle + color coding)
    Op nodeOp(int32_t nodeId) const;                // Op::Add for unknown id (documented)
    void setNodeOp(int32_t nodeId, Op op);          // marks scene lines dirty

    // scale tool — cumulative-delta semantics
    void beginScale();                              // captures the selected node's scale; no-op without selection
    void updateScale(float pixelDeltaY);            // scale = start_scale * exp(-pixelDeltaY * 0.005), per component,
                                                     // each component clamped to [0.05, 50]; no-op unless active; marks lines dirty
    void endScale();

    // node info (tests)
    Vec3f nodeScale(int32_t nodeId) const;          // {0,0,0} for unknown id

private:
    Editor();
    struct Impl;
    Impl* impl_;
};

} // namespace sq
