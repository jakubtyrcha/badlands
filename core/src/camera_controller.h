#pragma once
#include <simd/simd.h>

#include "camera.h"

namespace sq {

// --- world-axis pan: steep-pitch handling ----------------------------------
//
// Vertical pan moves along world +Y, which is what makes "drag up, travel up
// the body" hold at every camera angle — the property a Y-dominant subject
// like a character needs and a view-plane pan cannot give. The catch is that
// world Y is only ~3 degrees off the view axis at kPitchLimit, so at steep
// pitch an uncorrected vertical drag would push the target nearly straight at
// the camera and the screen would barely move. Two pure functions handle it.

// How much of the camera's own up vector to mix into the pan axis. 0 below
// ~50 degrees of pitch — pure world Y, where nearly all character work
// happens — easing to 1 by ~76, past which world Y is useless as a pan axis.
inline constexpr float kPanBlendBeginSin = 0.77f; // ~50 degrees
inline constexpr float kPanBlendEndSin   = 0.97f; // ~76 degrees
float pan_up_blend(float pitch);

// World-Y motion of length L only shows L*cos(pitch) on screen, so the drag is
// divided by cos(pitch) to keep tracking honest. The floor stops that
// exploding toward the pole, and the blend above retires the whole correction
// before the floor can bind — so this is exactly 1 at pitch 0 AND at full
// blend, and bounded by 1/kPanGainCosFloor in between.
inline constexpr float kPanGainCosFloor = 0.6f;
float pan_vertical_gain(float pitch);

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
    // Lowered from 0.5 for the cursor-anchored pivot: once the pivot sits ON
    // the surface being sculpted, this is the distance between the eye and the
    // skin, and 0.5 was a hard wall at nostril/eyelid scale on a 1-2 unit head.
    // Camera::kNear came down alongside it so geometry in front of the pivot
    // still isn't clipped at this range.
    static constexpr float kRadiusMin  = 0.15f;
    static constexpr float kRadiusMax  = 90.0f;        // below the 100 far plane
    static constexpr float kRadiusEps  = 1e-4f;
    // Dolly clamps the eye->focus distance rather than the radius: with the
    // pivot on a surface, radius stops meaning "how close am I to the thing I
    // am looking at", so it is the wrong quantity to bound a zoom with.
    static constexpr float kFocusDistMin = 0.15f;
    // Dolly rate per view point of drag. Its ratio to kZoomSens (per unit of
    // pinch magnification) is the whole of the points/magnification
    // conversion, which is why both live here rather than in the app layer.
    static constexpr float kDollySens  = 0.005f;

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

    // --- cursor-anchored navigation ---------------------------------------

    // Re-centres the orbit on `pivot` WITHOUT moving the eye: yaw/pitch/radius
    // are recomputed from the same eye position, so nothing on screen shifts
    // and only the meaning of a subsequent orbit() changes. That invariant is
    // the entire reason it is safe to re-derive the pivot at every gesture
    // rather than making the user place one.
    //
    // Returns false, state untouched, when it cannot honour that guarantee:
    //  - a non-finite pivot (see is_finite3 in math.h — a NaN here would
    //    corrupt the camera unrecoverably, so it is refused rather than clamped)
    //  - a pivot at the eye (no direction to decompose)
    //  - a pivot that would need a pitch past kPitchLimit; clamping the pitch
    //    would move the eye, breaking the one promise this makes. Reachable by
    //    aiming low on screen from an already-steep view; the caller simply
    //    keeps orbiting around the previous pivot.
    // Otherwise the eye->pivot DISTANCE is clamped into [kRadiusMin,
    // kRadiusMax] along the same direction, so a far pivot is pulled nearer
    // rather than discarded.
    bool set_pivot_preserving_eye(simd_float3 pivot);

    // World-axis pan. dx maps to the camera's right vector — already
    // horizontal in this rig, since right = normalize(cross(f, {0,1,0})) has
    // y == 0 for every pose, which is why only the vertical axis had to change
    // to make panning world-locked. dy maps to world +Y, blended toward camera
    // up at steep pitch (see pan_up_blend / pan_vertical_gain above).
    //
    // `depth` is the eye->focus distance, replacing radius in the points-to-
    // world scale: radius is only the right scale for content at the orbit
    // target, which is exactly what made the old pan overshoot when zoomed in
    // on a detail while the target sat at the model's centre.
    bool pan_world(float dx_pts, float dy_pts, float depth, float viewport_h_pts);

    // Dolly toward/away from `focus` as a rigid translation of the whole orbit
    // rig along the eye->focus ray:
    //     eye1 = focus + factor * (eye0 - focus),  target1 = target0 + (eye1 - eye0)
    // Radius and orientation are untouched, so `focus` provably projects to the
    // same pixel — zoom-to-cursor is exact here, not approximate.
    //
    // This deliberately leaves `target` sitting radius units ahead of the eye,
    // possibly well past the model; the next orbit's re-pivot resets it. The
    // two halves cover each other, which is why neither needs a correction of
    // its own. `factor` is exponential in drag distance at the call site, so
    // the approach to `focus` is already asymptotic and kFocusDistMin is a
    // backstop rather than something a normal gesture reaches.
    bool dolly_toward(simd_float3 focus, float factor);

    // Points of dolly drag equivalent to a cumulative pinch magnification, so
    // no sensitivity policy leaks into the app layer.
    static float dolly_points_for_magnification(float magnification) {
        return magnification * (kZoomSens / kDollySens);
    }

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
