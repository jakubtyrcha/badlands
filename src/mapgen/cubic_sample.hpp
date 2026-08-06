#pragma once

// Catmull-Rom POINT sampling of a Field2D at an arbitrary world position,
// with the analytic gradient. The raster path (ResampleField in
// coarse_world_patch_source.cpp) stays the producer of whole patches; this
// header exists for consumers that need value + slope at points BETWEEN
// output texels -- the relief filter's per-octave base queries.
//
// Conventions match CubicTaps exactly, or the two paths would disagree:
//   - NODE registration: source texel i IS world i*src_texel_m.
//   - Clamped-border taps, renormalised so the reconstruction keeps mass.
// Gradient is the derivative of the SAME renormalised weights (quotient
// rule), so it is exact everywhere including the clamped border -- and a
// constant field gets an exactly-zero gradient by construction.

#include <cmath>

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

constexpr int kCubicSupport = 2;  // taps per side

// Catmull-Rom -- the (B, C) = (0, 0.5) member of the Mitchell-Netravali
// cubic family, closed-form: interpolating (weight exactly 1 at zero offset,
// 0 at every other integer), mild overshoot. Shared by the raster resample
// and the point sampler; the choice over Lanczos-3 is argued at the raster
// site (coarse_world_patch_source.cpp: a linear ramp must reproduce
// exactly). Deliberately NOT the parameterized family: CatmullRomDeriv below
// is this exact polynomial's derivative, and a tunable (B, C) whose
// derivative silently does not follow would be a trap.
inline double CatmullRom(double x) {
  x = std::fabs(x);
  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

// d/dx of CatmullRom. The kernel is even, so the derivative is odd:
// differentiate the |x| polynomial and restore the sign.
inline double CatmullRomDeriv(double x) {
  const double s = x < 0.0 ? -1.0 : 1.0;
  x = std::fabs(x);
  if (x < 1.0) return s * (4.5 * x * x - 5.0 * x);
  if (x < 2.0) return s * (-1.5 * x * x + 5.0 * x - 4.0);
  return 0.0;
}

struct CubicSample {
  float value = 0.0f;
  glm::vec2 grad{0.0f};  // d(value)/d(world metres)
};

CubicSample cubic_sample(const Field2D<float>& src, float src_texel_m,
                         glm::dvec2 world_pos_m);

// Value-only variant: identical reconstruction, none of the derivative
// arithmetic. For callers that read .value alone (e.g. the relief filter's
// local-mean taps) the gradient work is pure waste.
float cubic_sample_value(const Field2D<float>& src, float src_texel_m,
                         glm::dvec2 world_pos_m);

}  // namespace badlands::mapgen
