#include "camera_controller.h"

#include <cmath>

#include "math.h" // is_finite3

namespace sq {

namespace {

float clampf(float v, float lo, float hi) {
    return std::fmax(lo, std::fmin(hi, v));
}

// Unit vector from target to eye for a given yaw/pitch (world +Y up).
simd_float3 direction(float yaw, float pitch) {
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    return simd_float3{cp * sy, sp, cp * cy};
}

} // namespace

CameraController CameraController::from_camera(const Camera& cam) {
    CameraController c;

    const simd_float3 offset = cam.eye - cam.target;
    const float len = simd_length(offset);

    float radius, pitch;
    if (len > kRadiusEps) {
        radius = len;
        pitch  = clampf(std::asin(offset.y / len), -kPitchLimit, kPitchLimit);
    } else {
        radius = kRadiusEps;
        pitch  = 0.0f;
    }

    c.target_        = cam.target;
    c.yaw_           = std::atan2(offset.x, offset.z); // atan2(0, 0) == 0, so safe when degenerate
    c.pitch_         = pitch;
    c.radius_        = radius;
    c.fov_y_radians_ = cam.fov_y_radians;
    c.aspect_        = cam.aspect;
    return c;
}

Camera CameraController::to_camera() const {
    Camera cam;
    cam.eye          = target_ + radius_ * direction(yaw_, pitch_);
    cam.target       = target_;
    cam.up           = simd_float3{0.0f, 1.0f, 0.0f};
    cam.fov_y_radians = fov_y_radians_;
    cam.aspect       = aspect_;
    return cam;
}

bool CameraController::orbit(float dx_pts, float dy_pts) {
    const float before_yaw = yaw_, before_pitch = pitch_;
    yaw_   = yaw_ + dx_pts * kOrbitSens;
    pitch_ = clampf(pitch_ - dy_pts * kOrbitSens, -kPitchLimit, kPitchLimit);
    return yaw_ != before_yaw || pitch_ != before_pitch;
}

bool CameraController::zoom(float delta) {
    if (!std::isfinite(delta)) {
        return false;
    }
    const float before = radius_;
    radius_ = clampf(radius_ * std::exp(-delta * kZoomSens), kRadiusMin, kRadiusMax);
    return radius_ != before;
}

bool CameraController::pan_view(float dx_pts, float dy_pts, float viewport_h_pts) {
    // Same f/s/u basis as Camera::view_proj's look-at matrix (world up is
    // always {0,1,0} for this controller).
    const simd_float3 dir      = direction(yaw_, pitch_); // target -> eye
    const simd_float3 f        = -dir;                    // eye -> target
    const simd_float3 world_up = simd_float3{0.0f, 1.0f, 0.0f};
    const simd_float3 right    = simd_normalize(simd_cross(f, world_up));
    const simd_float3 up       = simd_cross(right, f);

    const float scale = 2.0f * radius_ * std::tan(fov_y_radians_ * 0.5f) / viewport_h_pts;
    const simd_float3 step = (-dx_pts * right + dy_pts * up) * scale;

    if (step.x == 0.0f && step.y == 0.0f && step.z == 0.0f) {
        return false;
    }
    target_ += step;
    return true;
}

float pan_up_blend(float pitch) {
    const float s = std::fabs(std::sin(pitch));
    const float t = clampf((s - kPanBlendBeginSin) / (kPanBlendEndSin - kPanBlendBeginSin), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t); // smoothstep
}

float pan_vertical_gain(float pitch) {
    const float raw = 1.0f / std::fmax(std::fabs(std::cos(pitch)), kPanGainCosFloor);
    const float blend = pan_up_blend(pitch);
    // Lerp back to 1 as the axis becomes camera up, which needs no correction:
    // the two functions have to retire together or the gain would keep scaling
    // an axis that is already in the view plane.
    return raw + blend * (1.0f - raw);
}

bool CameraController::set_pivot_preserving_eye(simd_float3 pivot) {
    if (!is_finite3(pivot)) {
        return false;
    }

    const simd_float3 eye = to_camera().eye;
    const simd_float3 offset = eye - pivot; // pivot -> eye: the orbit arm
    const float len = simd_length(offset);
    // Negated comparison so a NaN length falls into the reject branch too
    // (every IEEE comparison against NaN is false).
    if (!(len > kRadiusEps)) {
        return false;
    }

    const simd_float3 dir = offset / len;
    const float pitch = std::asin(clampf(dir.y, -1.0f, 1.0f));
    if (std::fabs(pitch) > kPitchLimit) {
        return false; // see header: clamping here would move the eye
    }

    const float radius = clampf(len, kRadiusMin, kRadiusMax);
    // direction(yaw, pitch) reconstructs `dir` exactly from these two angles
    // (asin/atan2 are its inverse), so to_camera().eye comes back to `eye`
    // whatever the radius clamp did.
    target_ = eye - radius * dir;
    radius_ = radius;
    pitch_  = pitch;
    yaw_    = std::atan2(dir.x, dir.z);
    return true;
}

bool CameraController::pan_world(float dx_pts, float dy_pts, float depth, float viewport_h_pts) {
    if (!(viewport_h_pts > 0.0f) || !std::isfinite(depth) || !std::isfinite(dx_pts) ||
        !std::isfinite(dy_pts)) {
        return false;
    }

    // Same f/s/u basis as pan_view's, but only `right` is taken from it: the
    // vertical axis is world up, blended toward camera up near the pole.
    const simd_float3 dir      = direction(yaw_, pitch_); // target -> eye
    const simd_float3 f        = -dir;                    // eye -> target
    const simd_float3 world_up = simd_float3{0.0f, 1.0f, 0.0f};
    const simd_float3 right    = simd_normalize(simd_cross(f, world_up));
    const simd_float3 cam_up   = simd_cross(right, f);

    const float blend = pan_up_blend(pitch_);
    const simd_float3 mixed = world_up + blend * (cam_up - world_up);
    const float mixed_len = simd_length(mixed);
    // world_up and cam_up are never antiparallel (kPitchLimit keeps them under
    // 90 degrees apart), so the mix cannot cancel; the guard is for float
    // pathology only.
    const simd_float3 pan_up = (mixed_len > 1e-6f) ? (mixed / mixed_len) : cam_up;

    const float scale = 2.0f * std::fmax(depth, 0.0f) * std::tan(fov_y_radians_ * 0.5f) / viewport_h_pts;
    const simd_float3 step =
        (-dx_pts * right + dy_pts * pan_vertical_gain(pitch_) * pan_up) * scale;

    if (!is_finite3(step) || (step.x == 0.0f && step.y == 0.0f && step.z == 0.0f)) {
        return false;
    }
    target_ += step;
    return true;
}

bool CameraController::dolly_toward(simd_float3 focus, float factor) {
    if (!is_finite3(focus) || !std::isfinite(factor) || !(factor > 0.0f)) {
        return false;
    }

    const simd_float3 eye0 = to_camera().eye;
    const simd_float3 to_eye = eye0 - focus;
    const float d0 = simd_length(to_eye);
    if (!(d0 > kRadiusEps)) {
        return false; // eye already at the focus point: no ray to travel along
    }

    const float d1 = clampf(d0 * factor, kFocusDistMin, kRadiusMax);

    // Pure translation: yaw_/pitch_/radius_ are deliberately untouched, which
    // is what keeps `focus` on the same pixel.
    const simd_float3 eye1 = focus + (d1 / d0) * to_eye;
    const simd_float3 step = eye1 - eye0;

    // Test the actual displacement rather than d1 == d0. Once saturated at a
    // clamp, d0 is re-derived each call through target_ + radius*direction(),
    // so it drifts by an ulp or two and never compares equal to the clamp
    // constant — which would let a held gesture jitter the camera indefinitely
    // instead of resting. This also gives the honest "did it move?" answer the
    // other gesture methods promise.
    if (!is_finite3(step) || simd_length(step) <= kRadiusEps) {
        return false;
    }
    target_ += step;
    return true;
}

void CameraController::set_aspect(float aspect) {
    aspect_ = aspect;
}

} // namespace sq
