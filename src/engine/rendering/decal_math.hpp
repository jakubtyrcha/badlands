#pragma once

// CPU mirror of the projected-decal shader math (shaders/passes/decals.wesl).
//
// KEEP IN SYNC WITH shaders/passes/decals.wesl -- every function here is
// transcribed 1:1 into that shader. WESL and C++ cannot share code, so this is
// an unavoidable cross-language duplication (the same arrangement as
// frame.wesl's shadowNormalOffsetLength <-> ShadowMath::NormalOffsetLength).
// The point of the mirror is testability: this header is pure, header-only and
// GPU-free, so src/engine/tests/decal_math_tests.cpp can pin the SDFs, the
// perimeter parameterisation and the dash pattern with exact expected values
// before any of it reaches a shader.
//
// All functions work in the decal's LOCAL 2D frame: `local.x` is local X and
// `local.y` is local Z (the world XZ plane), origin at the decal centre, with
// the decal's yaw already un-rotated away.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/glm.hpp>

#include "engine/rendering/projected_decal.hpp"

namespace badlands::decal_math {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

// WGSL-identical helpers (spelled out rather than pulled from glm so the
// mirror is literal).
inline float Fract(float x) { return x - std::floor(x); }

inline float Smoothstep(float low, float high, float x) {
  const float denom = high - low;
  if (denom <= 0.0f) return x < low ? 0.0f : 1.0f;
  const float t = std::clamp((x - low) / denom, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// === Shape SDFs (distance to the outline CURVE, always >= 0) ===============

// Circle outline: distance from `local` to the circle of radius `radius`.
inline float RingOutlineDistance(glm::vec2 local, float radius) {
  return std::abs(glm::length(local) - radius);
}

// Signed distance to a rounded rectangle (negative inside). `half_extents` is
// the OUTER half-size; `radius` is the corner rounding, clamped to fit.
inline float RoundedRectSignedDistance(glm::vec2 local, glm::vec2 half_extents,
                                       float radius) {
  const float r = std::clamp(radius, 0.0f,
                             std::min(half_extents.x, half_extents.y));
  const glm::vec2 q = glm::abs(local) - (half_extents - glm::vec2(r));
  const glm::vec2 q_pos(std::max(q.x, 0.0f), std::max(q.y, 0.0f));
  return glm::length(q_pos) + std::min(std::max(q.x, q.y), 0.0f) - r;
}

// Rounded-rectangle outline distance (unsigned).
inline float RoundedRectOutlineDistance(glm::vec2 local, glm::vec2 half_extents,
                                        float radius) {
  return std::abs(RoundedRectSignedDistance(local, half_extents, radius));
}

// === Perimeters ============================================================

inline float RingPerimeter(float radius) { return kTwoPi * std::max(radius, 0.0f); }

inline float RoundedRectPerimeter(glm::vec2 half_extents, float radius) {
  const float r = std::clamp(radius, 0.0f,
                             std::min(half_extents.x, half_extents.y));
  const glm::vec2 b = glm::max(half_extents - glm::vec2(r), glm::vec2(0.0f));
  return 4.0f * b.x + 4.0f * b.y + kTwoPi * r;
}

// === Perimeter parameterisation (arc length along the outline) =============
//
// Both parameterisations run COUNTER-CLOCKWISE (in the local XY sense) and
// start at s = 0 on the +X axis, so a dash pattern is continuous across the
// wrap point once the period divides the perimeter (see FitDashPeriod).

// Arc length along the circle, in [0, 2*pi*radius).
inline float RingPerimeterParam(glm::vec2 local, float radius) {
  float angle = std::atan2(local.y, local.x);
  if (angle < 0.0f) angle += kTwoPi;
  return angle * std::max(radius, 0.0f);
}

// Arc length along the rounded rectangle, in [0, RoundedRectPerimeter).
//
// Walks CCW from s = 0 at the middle of the +X edge:
//   +X edge (upper half) -> +X+Y corner arc -> +Y edge -> -X+Y arc ->
//   -X edge -> -X-Y arc -> -Y edge -> +X-Y arc -> +X edge (lower half).
// A point strictly inside the inner box has no meaningful outline parameter
// (its outline coverage is zero there anyway) and returns 0.
inline float RoundedRectPerimeterParam(glm::vec2 local, glm::vec2 half_extents,
                                       float radius) {
  const float r = std::clamp(radius, 0.0f,
                             std::min(half_extents.x, half_extents.y));
  const glm::vec2 b = glm::max(half_extents - glm::vec2(r), glm::vec2(0.0f));
  const float arc = r * kHalfPi;           // one quarter-circle
  const float perimeter = 4.0f * b.x + 4.0f * b.y + kTwoPi * r;

  // Cumulative arc length at the end of each segment.
  const float s_right_upper = b.y;                             // +X edge, upper half
  const float s_corner_pp = s_right_upper + arc;               // +X+Y arc
  const float s_top = s_corner_pp + 2.0f * b.x;                // +Y edge
  const float s_corner_mp = s_top + arc;                       // -X+Y arc
  const float s_left = s_corner_mp + 2.0f * b.y;               // -X edge
  const float s_corner_mm = s_left + arc;                      // -X-Y arc
  const float s_bottom = s_corner_mm + 2.0f * b.x;             // -Y edge
  const float s_corner_pm = s_bottom + arc;                    // +X-Y arc

  // Inclusive comparisons matter: at radius 0 the outline IS the inner box
  // boundary, so every outline point sits exactly at |local| == b. Strict
  // comparisons would drop the whole sharp-cornered outline into the interior
  // case below and return 0 everywhere. Where two segments meet, both branches
  // agree on the boundary value (pinned by the segment-boundary test), so the
  // branch order is free to pick either.
  const bool right = local.x >= b.x;
  const bool left = local.x <= -b.x;
  const bool up = local.y >= b.y;
  const bool down = local.y <= -b.y;

  if (right && up) {  // +X+Y corner arc: angle 0 -> pi/2
    const glm::vec2 d = local - b;
    const float theta = std::atan2(d.y, d.x);
    return s_right_upper + r * std::clamp(theta, 0.0f, kHalfPi);
  }
  if (left && up) {  // -X+Y corner arc: angle pi/2 -> pi
    const glm::vec2 d = local - glm::vec2(-b.x, b.y);
    const float theta = std::atan2(d.y, d.x);
    return s_top + r * (std::clamp(theta, kHalfPi, kPi) - kHalfPi);
  }
  if (left && down) {  // -X-Y corner arc: angle pi -> 3pi/2
    const glm::vec2 d = local - glm::vec2(-b.x, -b.y);
    float theta = std::atan2(d.y, d.x);
    if (theta < 0.0f) theta += kTwoPi;
    return s_left + r * (std::clamp(theta, kPi, kPi + kHalfPi) - kPi);
  }
  if (right && down) {  // +X-Y corner arc: angle 3pi/2 -> 2pi
    const glm::vec2 d = local - glm::vec2(b.x, -b.y);
    float theta = std::atan2(d.y, d.x);
    // This arc ENDS at the wrap seam, where atan2 returns 0 rather than 2*pi.
    // `d.y <= 0` holds throughout this branch, so theta is in [-pi, 0] and
    // shifting on <= 0 (not < 0) maps the seam to 2*pi -- which is what makes
    // this branch agree with the +X-edge branch that also claims that point.
    if (theta <= 0.0f) theta += kTwoPi;
    return s_bottom +
           r * (std::clamp(theta, kPi + kHalfPi, kTwoPi) - (kPi + kHalfPi));
  }
  if (right) {  // +X edge: s = 0 at local.y = 0, wrapping through the end
    return local.y >= 0.0f ? local.y : perimeter + local.y;
  }
  if (up) {  // +Y edge, running -X-ward
    return s_corner_pp + (b.x - local.x);
  }
  if (left) {  // -X edge, running -Y-ward
    return s_corner_mp + (b.y - local.y);
  }
  if (down) {  // -Y edge, running +X-ward
    return s_corner_mm + (local.x + b.x);
  }
  return 0.0f;  // interior: no outline here
}

// === Dash pattern ==========================================================

// Snaps `period` to the nearest whole number of repeats around a CLOSED curve
// of length `perimeter`, so the pattern joins seamlessly at the wrap point.
// Degenerate inputs pass through unchanged.
//
// Rounds with floor(x + 0.5) rather than round(): std::round breaks ties away
// from zero while WGSL's round() breaks them to even, so using round() on both
// sides would make this mirror and the shader disagree on exactly-.5 repeat
// counts. floor(x + 0.5) is defined identically in C++ and WGSL.
inline float FitDashPeriod(float period, float perimeter) {
  if (period <= 0.0f || perimeter <= 0.0f) return period;
  const float n = std::max(1.0f, std::floor(perimeter / period + 0.5f));
  return perimeter / n;
}

// Fraction of each period covered by the "a" dash, in [0, 1].
inline float DashDuty(float dash_length, float dash_gap) {
  const float period = dash_length + dash_gap;
  if (period <= 0.0f) return 1.0f;
  return std::clamp(dash_length / period, 0.0f, 1.0f);
}

// True when arc-length `s` lands on an "a" dash. `period` must already be
// fitted (FitDashPeriod). The pattern scrolls along +s at `scroll` periods per
// second.
inline bool IsDashA(float s, float period, float duty, float time,
                    float scroll) {
  if (period <= 0.0f || duty >= 1.0f) return true;
  if (duty <= 0.0f) return false;
  const float phase = Fract((s - time * scroll * period) / period);
  return phase < duty;
}

// === Coverage ==============================================================

// Antialiased outline coverage: 1 on the line's centreline, falling to 0 over
// an `aa` half-width band outside `half_width`.
inline float OutlineCoverage(float outline_distance, float half_width,
                             float aa) {
  const float e = std::max(aa, 1e-6f);
  return 1.0f - Smoothstep(half_width - e, half_width + e, outline_distance);
}

// === CPU-only helpers (NOT mirrored in the shader) =========================

// Pixel rect, as wgpu::RenderPassEncoder::SetScissorRect takes it.
struct ScissorRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Conservative screen-space bounds of every decal's projector volume.
//
// The decal pass draws a fullscreen triangle, but a handful of small decals
// only ever touch a tiny part of the screen -- without a scissor every pixel
// pays for the depth load, world reconstruction and SDF evaluation. Each decal
// is bounded by the box (centre +/- radius in XZ, +/- projector_half_height in
// Y), where radius is |half_extents| + half the line width: that bound is
// rotation-independent, so yaw needs no special handling. Projection is a
// projective map, so for a box entirely in front of the eye the image of the
// corners bounds the image of the box.
//
// `view_proj` is world -> clip. Returns false when the bounds cannot be
// trusted -- no decals, a degenerate viewport, or any corner at/behind the eye
// plane (where the perspective divide flips and a bounding box is meaningless).
// The caller must then shade the FULL viewport: a false here must never be
// mistaken for "nothing to draw". A rect of zero width or height means the
// decals are genuinely off-screen.
inline bool ComputeDecalScissor(const ProjectedDecal* decals, uint32_t count,
                                const glm::mat4& view_proj, uint32_t width,
                                uint32_t height, ScissorRect& out) {
  if (decals == nullptr || count == 0 || width == 0 || height == 0) {
    return false;
  }

  float min_x = std::numeric_limits<float>::max();
  float min_y = std::numeric_limits<float>::max();
  float max_x = std::numeric_limits<float>::lowest();
  float max_y = std::numeric_limits<float>::lowest();

  for (uint32_t i = 0; i < count; ++i) {
    const ProjectedDecal& d = decals[i];
    const float radius =
        glm::length(d.half_extents) + 0.5f * std::max(d.line_width, 0.0f);
    const float band = std::max(d.projector_half_height, 0.0f);

    for (int corner = 0; corner < 8; ++corner) {
      const glm::vec3 offset((corner & 1) ? radius : -radius,
                             (corner & 2) ? band : -band,
                             (corner & 4) ? radius : -radius);
      const glm::vec4 clip = view_proj * glm::vec4(d.center + offset, 1.0f);
      if (clip.w <= 1e-4f) return false;  // spans the eye plane: bail out
      const glm::vec3 ndc = glm::vec3(clip) / clip.w;
      const float px = (ndc.x * 0.5f + 0.5f) * static_cast<float>(width);
      const float py =
          (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(height);
      min_x = std::min(min_x, px);
      max_x = std::max(max_x, px);
      min_y = std::min(min_y, py);
      max_y = std::max(max_y, py);
    }
  }

  // Two pixels of slack for the shader's antialiasing band, then clamp to the
  // attachment (an off-screen decal collapses to a zero-size rect).
  const float fw = static_cast<float>(width);
  const float fh = static_cast<float>(height);
  const float x0 = std::clamp(std::floor(min_x) - 2.0f, 0.0f, fw);
  const float y0 = std::clamp(std::floor(min_y) - 2.0f, 0.0f, fh);
  const float x1 = std::clamp(std::ceil(max_x) + 2.0f, 0.0f, fw);
  const float y1 = std::clamp(std::ceil(max_y) + 2.0f, 0.0f, fh);

  out.x = static_cast<uint32_t>(x0);
  out.y = static_cast<uint32_t>(y0);
  out.width = static_cast<uint32_t>(x1 - x0);
  out.height = static_cast<uint32_t>(y1 - y0);
  return true;
}

}  // namespace badlands::decal_math
