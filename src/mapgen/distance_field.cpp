#include "mapgen/distance_field.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

// One 1D pass of the Felzenszwalb–Huttenlocher squared-distance transform:
// given f[i] = best squared WORLD distance already achieved at sample i
// (kBigD = "no seed"), writes d[i] = min_j(f[j] + (step*(i-j))^2) via the
// parabola lower envelope. Double precision so the exact-vs-brute-force
// test guarantee holds at map-scale magnitudes. kBigD is a large FINITE
// value, not infinity: two "empty" parabolas must intersect at a finite
// point or the envelope math produces NaN.
constexpr double kBigD = 1e30;

void dt1d(const std::vector<double>& f, std::vector<double>& d,
          std::vector<int>& v, std::vector<double>& z, int n, double step) {
  const double s2 = step * step;
  int k = 0;
  v[0] = 0;
  z[0] = -kBigD;
  z[1] = kBigD;
  for (int q = 1; q < n; ++q) {
    const double fq = f[q] + s2 * q * q;
    for (;;) {
      const int p = v[k];
      const double s =
          (fq - (f[p] + s2 * p * p)) / (2.0 * s2 * static_cast<double>(q - p));
      if (k > 0 && s <= z[k]) {
        --k;
        continue;
      }
      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = kBigD;
      break;
    }
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < static_cast<double>(q)) ++k;
    const int p = v[k];
    const double dq = step * static_cast<double>(q - p);
    d[q] = f[p] + dq * dq;
  }
}

}  // namespace

Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask,
                                glm::vec2 texel_m) {
  const int w = mask.width, h = mask.height;
  if (w <= 0 || h <= 0) return Field2D<float>{};
  Field2D<float> out(w, h, 0.0f);

  bool any_seed = false;
  for (uint8_t m : mask.data) {
    if (m != 0) {
      any_seed = true;
      break;
    }
  }
  if (!any_seed) return out;  // documented degenerate: all zeros

  // Squared world distances between the two passes.
  std::vector<double> g(static_cast<size_t>(w) * h);

  struct Scratch {
    std::vector<double> f, d, z;
    std::vector<int> v;
  };
  const int n_max = std::max(w, h);
  auto make_scratch = [n_max] {
    Scratch s;
    s.f.resize(static_cast<size_t>(n_max));
    s.d.resize(static_cast<size_t>(n_max));
    s.z.resize(static_cast<size_t>(n_max) + 1);
    s.v.resize(static_cast<size_t>(n_max));
    return s;
  };

  // Pass 1: per COLUMN over y (step = texel_m.y). Columns are independent;
  // parallel_tiles with height 1 hands out x-ranges.
  parallel_tiles(w, 1, 64, make_scratch,
                 [&](Scratch& s, int x0, int, int x1, int) {
                   for (int x = x0; x < x1; ++x) {
                     for (int y = 0; y < h; ++y)
                       s.f[y] = mask.at(x, y) != 0 ? 0.0 : kBigD;
                     dt1d(s.f, s.d, s.v, s.z, h, texel_m.y);
                     for (int y = 0; y < h; ++y)
                       g[static_cast<size_t>(y) * w + x] = s.d[y];
                   }
                 });

  // Pass 2: per ROW over x (step = texel_m.x) on pass 1's result; sqrt out.
  parallel_tiles(h, 1, 64, make_scratch,
                 [&](Scratch& s, int y0, int, int y1, int) {
                   for (int y = y0; y < y1; ++y) {
                     for (int x = 0; x < w; ++x)
                       s.f[x] = g[static_cast<size_t>(y) * w + x];
                     dt1d(s.f, s.d, s.v, s.z, w, texel_m.x);
                     for (int x = 0; x < w; ++x)
                       out.at(x, y) = static_cast<float>(std::sqrt(s.d[x]));
                   }
                 });

  return out;
}

}  // namespace badlands::mapgen
