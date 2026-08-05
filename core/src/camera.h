#pragma once
#include <simd/simd.h>

namespace sq {

struct Ray {
    simd_float3 origin;
    simd_float3 dir;      // normalized
};

struct ViewPoint {
    float x, y;           // view points, top-left origin
    bool visible;         // false when the world point is at/behind the camera plane (clip w <= 0)
};

struct Camera {
    simd_float3 eye;
    simd_float3 target;
    simd_float3 up;               // {0,1,0}
    float fov_y_radians;
    float aspect;

    // Lowered from 0.1 alongside CameraController::kRadiusMin: with the orbit
    // pivot anchored on the surface being sculpted, the eye can now sit 0.15
    // from the skin, and a 0.1 near plane would clip away everything between.
    // Widens the depth ratio to 2000:1 — tolerable because the raymarch writes
    // true per-pixel depth into a Depth32Float target, but it is the thing to
    // watch if the ground plate ever z-fights with geometry resting on it.
    static constexpr float kNear = 0.05f;
    static constexpr float kFar  = 100.0f;

    simd_float4x4 view_proj() const;
    Ray ray_through_view_point(float x, float y, float w_pts, float h_pts) const;
    ViewPoint project(simd_float3 world, float w_pts, float h_pts) const;
};

} // namespace sq
