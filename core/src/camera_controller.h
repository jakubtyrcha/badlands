#pragma once
#include <simd/simd.h>

#include "camera.h"

namespace sq {

// Orbit-camera controller: turns pointer/keyboard gestures into camera
// moves. Pure math, no windowing/AppKit/Metal — the input plumbing lives in
// the app layer (Editor forwards to this).
//
// State is kept in spherical coordinates around the orbit `target`. World up
// is assumed to be +Y, so eye = target + radius * (cos(pitch)*sin(yaw),
// sin(pitch), cos(pitch)*cos(yaw)). Keeping yaw/pitch/radius rather than a
// raw eye position means orbiting and zooming never drift, and the pitch
// clamp that stops the view flipping through the pole is trivial.
//
// Every gesture method returns whether it actually moved the camera, so the
// caller can skip redundant work for no-op input (a zero delta, or a
// drag/zoom already saturated at a clamp).
class CameraController {
public:
    static constexpr float kOrbitSens  = 0.005f;      // radians per point of drag
    static constexpr float kZoomSens   = 0.5f;         // exponential zoom rate per unit pinch
    static constexpr float kPitchLimit = 1.5207964f;   // pi/2 - 0.05
    static constexpr float kRadiusMin  = 0.5f;
    static constexpr float kRadiusMax  = 90.0f;        // below the 100 far plane
    static constexpr float kRadiusEps  = 1e-4f;

    // Decomposes a look-at camera into orbit state. A degenerate eye ==
    // target decodes to a valid tiny-radius orbit rather than NaNs; an
    // over-steep incoming pitch is clamped so attaching the controller never
    // causes a jump on the first drag.
    static CameraController from_camera(const Camera& cam);

    // Recomposes the orbit state into a look-at camera. Keeps fov/aspect and
    // sets up = {0,1,0}.
    Camera to_camera() const;

    // Rotates the eye around the target. dx/dy are raw pointer deltas in
    // view points (y grows downward): dx>0 increases yaw, dy<0 (drag up in
    // flipped coords) increases pitch (camera rises). Pitch is clamped short
    // of the pole. Returns whether the camera moved.
    bool orbit(float dx_pts, float dy_pts);

    // Dollies along the view ray. Positive delta (pinch-out) zooms in:
    // radius *= exp(-delta * kZoomSens), clamped to [kRadiusMin, kRadiusMax].
    // Returns whether the radius changed.
    bool zoom(float delta);

    // Screen-space pan (deviation from the ported Rust original's world-XZ
    // pan): target += (-dx * right + dy * up) * (2 * radius * tan(fov_y/2) /
    // viewport_h_pts), where right/up are the camera basis vectors (same
    // s/u as the view matrix). Content tracks the fingers 1:1 at target
    // depth; view coords have a top-left origin. Returns whether the target
    // moved.
    bool pan_view(float dx_pts, float dy_pts, float viewport_h_pts);

    void set_aspect(float aspect);

    // Accessors for tests.
    simd_float3 target() const { return target_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float radius() const { return radius_; }

private:
    simd_float3 target_;
    float yaw_, pitch_, radius_;
    float fov_y_radians_, aspect_;
};

} // namespace sq
