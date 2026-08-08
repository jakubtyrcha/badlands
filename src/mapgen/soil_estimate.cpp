#include "mapgen/soil_estimate.hpp"

#include <algorithm>
#include <cmath>

namespace badlands::mapgen {

namespace {

// soil(s) = A * exp(-s / B), with A and B solved from the two measured points
// so the curve passes through both exactly:
//
//     B = (s_thin - s_deep) / ln(d_deep / d_thin)
//     A = d_deep * exp(s_deep / B)
//
// Derived rather than written down as 7.80 and 10.64, so that moving a
// measurement moves the curve instead of leaving two stale magic numbers.
struct Fit {
  float a, b;
};

Fit soil_fit() {
  const float b = (kMeasuredThinSlopeDeg - kMeasuredDeepSlopeDeg) /
                  std::log(kMeasuredDeepSoilM / kMeasuredThinSoilM);
  const float a = kMeasuredDeepSoilM * std::exp(kMeasuredDeepSlopeDeg / b);
  return {a, b};
}

}  // namespace

Field2D<float> estimate_soil(const Field2D<float>& height, float texel_m) {
  const int w = height.width, h = height.height;
  if (w <= 0 || h <= 0 || texel_m <= 0.0f) return {};

  const Fit fit = soil_fit();
  Field2D<float> soil(w, h, 0.0f);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Central difference, clamped at the border. The border is one texel of a
      // one-sided estimate, which is fine here: the fit's own uncertainty
      // dwarfs the difference.
      //
      // On a raster one texel wide the clamps collapse (x1 == x0) and the
      // divisor is zero, so the guard is not decorative -- 0/0 is NaN, and a
      // NaN soil value reaches disk through write_patch and the splat through
      // BuildGroundSplat, where a NaN compares false against every bound.
      const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, w - 1);
      const int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, h - 1);
      const float dzdx =
          x1 == x0 ? 0.0f
                   : (height.at(x1, y) - height.at(x0, y)) /
                         (static_cast<float>(x1 - x0) * texel_m);
      const float dzdy =
          y1 == y0 ? 0.0f
                   : (height.at(x, y1) - height.at(x, y0)) /
                         (static_cast<float>(y1 - y0) * texel_m);

      const float slope_deg =
          std::atan(std::hypot(dzdx, dzdy)) * (180.0f / 3.14159265358979f);
      soil.at(x, y) =
          std::clamp(fit.a * std::exp(-slope_deg / fit.b), 0.0f, fit.a);
    }
  }
  return soil;
}

}  // namespace badlands::mapgen
