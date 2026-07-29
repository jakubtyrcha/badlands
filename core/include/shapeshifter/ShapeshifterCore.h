#pragma once
#include <cstdint>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace sq {

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

private:
    Editor();
    struct Impl;
    Impl* impl_;
};

} // namespace sq
