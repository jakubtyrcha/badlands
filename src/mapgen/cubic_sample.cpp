#include "mapgen/cubic_sample.hpp"

#include <algorithm>

namespace badlands::mapgen {

namespace {

// One axis's 4 clamped taps at u (source-texel units): renormalised weights
// W_i and their derivatives dW_i/du. With S = sum(w) and S' = sum(w'), the
// quotient rule gives dW_i = (w_i' * S - w_i * S') / S^2 -- which makes
// sum(dW_i) identically zero, so a constant field always gets zero slope.
struct AxisTaps {
  int idx[2 * kCubicSupport];
  double w[2 * kCubicSupport];
  double dw[2 * kCubicSupport];
};

AxisTaps axis_taps(double u, int src_n) {
  AxisTaps t;
  const int base = static_cast<int>(std::floor(u)) - kCubicSupport + 1;
  double sum = 0.0, dsum = 0.0;
  for (int c = 0; c < 2 * kCubicSupport; ++c) {
    const int i = base + c;
    t.idx[c] = std::clamp(i, 0, src_n - 1);
    t.w[c] = CatmullRom(u - i);
    t.dw[c] = CatmullRomDeriv(u - i);
    sum += t.w[c];
    dsum += t.dw[c];
  }
  if (sum != 0.0) {
    const double inv = 1.0 / sum;
    for (int c = 0; c < 2 * kCubicSupport; ++c) {
      t.dw[c] = (t.dw[c] * sum - t.w[c] * dsum) * inv * inv;
      t.w[c] *= inv;
    }
  }
  return t;
}

}  // namespace

float cubic_sample_value(const Field2D<float>& src, float src_texel_m,
                         glm::dvec2 world_pos_m) {
  if (src.width <= 0 || src.height <= 0 || src_texel_m <= 0.0f) return 0.0f;
  const double texel = static_cast<double>(src_texel_m);
  const auto axis = [](double u, int src_n, int* idx, double* w) {
    const int base = static_cast<int>(std::floor(u)) - kCubicSupport + 1;
    double sum = 0.0;
    for (int c = 0; c < 2 * kCubicSupport; ++c) {
      const int i = base + c;
      idx[c] = std::clamp(i, 0, src_n - 1);
      w[c] = CatmullRom(u - i);
      sum += w[c];
    }
    if (sum != 0.0)
      for (int c = 0; c < 2 * kCubicSupport; ++c) w[c] /= sum;
  };
  int ix[2 * kCubicSupport], iy[2 * kCubicSupport];
  double wx[2 * kCubicSupport], wy[2 * kCubicSupport];
  axis(world_pos_m.x / texel, src.width, ix, wx);
  axis(world_pos_m.y / texel, src.height, iy, wy);
  double v = 0.0;
  for (int b = 0; b < 2 * kCubicSupport; ++b) {
    double row = 0.0;
    for (int a = 0; a < 2 * kCubicSupport; ++a)
      row += wx[a] * src.at(ix[a], iy[b]);
    v += wy[b] * row;
  }
  return static_cast<float>(v);
}

CubicSample cubic_sample(const Field2D<float>& src, float src_texel_m,
                         glm::dvec2 world_pos_m) {
  if (src.width <= 0 || src.height <= 0 || src_texel_m <= 0.0f) return {};
  const double texel = static_cast<double>(src_texel_m);
  const AxisTaps tx = axis_taps(world_pos_m.x / texel, src.width);
  const AxisTaps ty = axis_taps(world_pos_m.y / texel, src.height);

  double v = 0.0, dx = 0.0, dy = 0.0;
  for (int b = 0; b < 2 * kCubicSupport; ++b) {
    double row_v = 0.0, row_d = 0.0;
    for (int a = 0; a < 2 * kCubicSupport; ++a) {
      const double f = src.at(tx.idx[a], ty.idx[b]);
      row_v += tx.w[a] * f;
      row_d += tx.dw[a] * f;
    }
    v += ty.w[b] * row_v;
    dx += ty.w[b] * row_d;
    dy += ty.dw[b] * row_v;
  }
  // dw is per source texel; world gradients need the 1/texel_m chain factor.
  return {static_cast<float>(v),
          {static_cast<float>(dx / texel), static_cast<float>(dy / texel)}};
}

}  // namespace badlands::mapgen
