#include "camera_controller.h"

#include <cmath>

#include "math_util.h" // is_finite3

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

// Shrinks `delta` so `angle + delta` stays within the pole clamp, never
// reversing or creating motion. Deliberately NOT
// `clamp(angle+delta) - angle`: an angle sitting a hair outside the limit
// (float noise left by an earlier rotation) would make that expression return
// a small correction even for delta == 0, so a gesture that asked for nothing
// would nudge the camera and report that it moved.
float clamp_pitch_delta(float angle, float delta) {
    if (delta == 0.0f) {
        return 0.0f;
    }
    const float allowed = clampf(angle + delta, -CameraController::kPitchLimit,
                                 CameraController::kPitchLimit) - angle;
    return (delta > 0.0f) ? std::fmax(0.0f, std::fmin(allowed, delta))
                          : std::fmin(0.0f, std::fmax(allowed, delta));
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
    c.pivot_         = cam.target; // coincident until a gesture re-derives it
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
    const float d_yaw = dx_pts * kOrbitSens;

    // Decompose the arm (pivot -> eye) the same way the view is decomposed. If
    // the pivot IS the look-at target these two angle pairs are equal by
    // construction, so everything below collapses to the original
    // "rotate the view" behaviour.
    const simd_float3 eye0 = to_camera().eye;
    const simd_float3 arm = eye0 - pivot_;
    const float arm_len = simd_length(arm);
    const bool have_arm = arm_len > kRadiusEps;
    float arm_yaw = 0.0f, arm_pitch = 0.0f;
    if (have_arm) {
        arm_yaw   = std::atan2(arm.x, arm.z);
        arm_pitch = std::asin(clampf(arm.y / arm_len, -1.0f, 1.0f));
    }

    // Shrink the pitch delta so NEITHER angle crosses the pole. Letting one
    // saturate while the other kept turning would desync the arm from the
    // view, and the camera would slowly drift its aim over a long drag.
    float d_pitch = -dy_pts * kOrbitSens;
    d_pitch = clamp_pitch_delta(pitch_, d_pitch);
    if (have_arm) {
        d_pitch = clamp_pitch_delta(arm_pitch, d_pitch);
    }

    if (d_yaw == 0.0f && d_pitch == 0.0f) {
        return false;
    }

    yaw_   += d_yaw;
    pitch_ += d_pitch;

    // Only when the pivot is genuinely off the look-at target: there, the eye
    // has to be re-placed from the rotated arm and target_ re-derived to match.
    // When the two coincide the yaw_/pitch_ update above already swings the eye
    // around the target correctly, and running the round trip anyway would feed
    // a float ulp of drift into target_ on every single call.
    if (have_arm && simd_distance_squared(pivot_, target_) > kRadiusEps * kRadiusEps) {
        // radius_ is untouched: the rotation is rigid.
        const simd_float3 eye1 = pivot_ + arm_len * direction(arm_yaw + d_yaw, arm_pitch + d_pitch);
        target_ = eye1 - radius_ * direction(yaw_, pitch_);
    }
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
        return false; // a NaN pivot would corrupt the camera unrecoverably
    }
    // Deliberately the whole implementation. Nothing observable changes here:
    // the eye, the view direction, target_ and radius_ are all untouched, so
    // re-deriving the pivot at every gesture start is invisible on screen.
    pivot_ = pivot;
    return true;
}

bool CameraController::pan_world(float dx_pts, float dy_pts, float depth, float viewport_h_pts) {
    if (!(viewport_h_pts > 0.0f) || !std::isfinite(depth) || !std::isfinite(dx_pts) ||
        !std::isfinite(dy_pts)) {
        return false;
    }

    // Same f/s/u basis Camera::view_proj's look-at matrix builds (world up is
    // always {0,1,0} here), but only `right` is taken from it: the vertical
    // axis is world up, blended toward camera up near the pole.
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
    pivot_  += step; // the rig translates as a whole
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
    pivot_  += step; // the rig translates as a whole
    return true;
}

bool CameraController::frame_on(simd_float3 target, float radius) {
    if (!is_finite3(target) || !std::isfinite(radius)) {
        return false;
    }
    target_ = target;
    pivot_  = target; // framing re-centres both: the framed node IS the new orbit centre
    radius_ = clampf(radius, kRadiusMin, kRadiusMax);
    return true; // yaw_/pitch_ untouched by design
}

void CameraController::set_aspect(float aspect) {
    aspect_ = aspect;
}

} // namespace sq
