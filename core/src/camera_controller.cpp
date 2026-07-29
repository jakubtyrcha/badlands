#include "camera_controller.h"

#include <cmath>

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

void CameraController::set_aspect(float aspect) {
    aspect_ = aspect;
}

} // namespace sq
