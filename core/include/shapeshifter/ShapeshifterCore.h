#pragma once
#include <cstdint>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace sq {

struct Vec3f { float x, y, z; };

// Miss/no-selection sentinel for the picking & selection API below. This is
// the same -1 sentinel core/src/scene.h's SceneDocument already uses for
// "no id" (Node::id, Node::snap_parent) — deliberately not redeclared here as
// a same-named `sq::kInvalidNode` constant: scene.h is a private core/src
// header not on the Swift-visible include path, but Editor.cpp (and any test
// that exercises both the picking module and SceneDocument directly) include
// both headers in one translation unit, and two `inline constexpr` variables
// with the same fully-qualified name in the same TU is an ODR redefinition
// error even when byte-identical. Swift call sites never reference the
// symbol by name (they just thread PickResult.node_id through), so nothing
// is lost by leaving the single definition in scene.h.
struct PickResult {
    int32_t node_id;   // -1 (SceneDocument's kInvalidNode) on miss
    Vec3f point;
    Vec3f normal;
};

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

private:
    Editor();
    struct Impl;
    Impl* impl_;
};

} // namespace sq
