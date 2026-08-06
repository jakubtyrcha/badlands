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

// Mitchell-Netravali cubic family. (B, C) = (0, 0.5) is Catmull-Rom --
// interpolating (weight exactly 1 at zero offset, 0 at every other integer),
// mild overshoot. Shared by the raster resample and the point sampler; the
// choice over Lanczos-3 is argued at the raster site
// (coarse_world_patch_source.cpp: a linear ramp must reproduce exactly).
inline double CubicWeight(double x, double B, double C) {
  x = std::fabs(x);
  const double x2 = x * x, x3 = x2 * x;
  if (x < 1.0)
    return ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 +
            (6 - 2 * B)) /
           6.0;
  if (x < 2.0)
    return ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x +
            (8 * B + 24 * C)) /
           6.0;
  return 0.0;
}

constexpr int kCubicSupport = 2;  // taps per side

inline double CatmullRom(double x) { return CubicWeight(x, 0.0, 0.5); }

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

}  // namespace badlands::mapgen
