#pragma once

// World-aligned bilinear resampling: reads a source Field2D at the
// destination grid's world positions (not texel-index positions), so a
// different resolution or origin resamples correctly. Used to go from the
// (possibly padded/offset) erosion sim grid to the output heightmap grid.

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Sample src (texel spacing src_texel_m, texel (0,0) at world src_origin_m,
// square texels on both axes) at the output grid's world positions (texel x
// at world x * dst_texel_m), bilinear, clamped at src edges.
inline Field2D<float> resample_bilinear(const Field2D<float>& src,
                                        float src_texel_m, float src_origin_m,
                                        int dst_res, float dst_texel_m) {
  Field2D<float> out(dst_res, dst_res, 0.0f);
  if (src.width <= 0 || src.height <= 0 || dst_res <= 0) return out;
  for (int y = 0; y < dst_res; ++y) {
    for (int x = 0; x < dst_res; ++x) {
      const float sx = (static_cast<float>(x) * dst_texel_m - src_origin_m) / src_texel_m;
      const float sy = (static_cast<float>(y) * dst_texel_m - src_origin_m) / src_texel_m;
      const float cx = std::clamp(sx, 0.0f, static_cast<float>(src.width - 1));
      const float cy = std::clamp(sy, 0.0f, static_cast<float>(src.height - 1));
      const int x0 = static_cast<int>(cx), y0 = static_cast<int>(cy);
      const int x1 = std::min(x0 + 1, src.width - 1);
      const int y1 = std::min(y0 + 1, src.height - 1);
      const float fx = cx - x0, fy = cy - y0;
      const float a = src.at(x0, y0) * (1 - fx) + src.at(x1, y0) * fx;
      const float b = src.at(x0, y1) * (1 - fx) + src.at(x1, y1) * fx;
      out.at(x, y) = a * (1 - fy) + b * fy;
    }
  }
  return out;
}

// World-aligned MAX-POOL resampling: unlike resample_bilinear (a point
// sample, right for smoothly-varying fields), this takes the MAX over every
// source texel that falls within the destination texel's Voronoi half-width
// — crisp, no smear, for thin high-contrast features (the river artifact).
// Same world-space node convention as resample_bilinear (texel x at world
// x*texel_m + origin_m); when dst is coarser than src this is a real pool
// over several covering src texels, when dst is finer it degenerates to
// nearest-neighbor.
inline Field2D<float> resample_max_pool(const Field2D<float>& src,
                                        float src_texel_m, float src_origin_m,
                                        int dst_res, float dst_texel_m) {
  Field2D<float> out(dst_res, dst_res, 0.0f);
  if (src.width <= 0 || src.height <= 0 || dst_res <= 0) return out;
  // Half the destination texel's footprint, in SOURCE texel units.
  const float half_span = 0.5f * dst_texel_m / src_texel_m;
  auto span_for = [&](int i, int extent) {
    const float world = static_cast<float>(i) * dst_texel_m;
    const float sx = (world - src_origin_m) / src_texel_m;
    int lo = static_cast<int>(std::ceil(sx - half_span));
    int hi = static_cast<int>(std::ceil(sx + half_span)) - 1;
    if (hi < lo) {  // degenerate: footprint narrower than the src spacing and
      // straddling a src texel's boundary (upsampling) — fall back to the
      // single nearest src node instead of covering nothing.
      lo = hi = static_cast<int>(std::lround(sx));
    }
    lo = std::clamp(lo, 0, extent - 1);
    hi = std::clamp(hi, 0, extent - 1);
    return std::pair<int, int>{lo, hi};
  };
  std::vector<std::pair<int, int>> xs(static_cast<size_t>(dst_res));
  std::vector<std::pair<int, int>> ys(static_cast<size_t>(dst_res));
  for (int x = 0; x < dst_res; ++x) xs[static_cast<size_t>(x)] = span_for(x, src.width);
  for (int y = 0; y < dst_res; ++y) ys[static_cast<size_t>(y)] = span_for(y, src.height);
  // Terminal texels absorb the residual source bands: node-centered windows
  // otherwise never pool source content beyond the last node's half-span, so
  // a sparse feature (a river line) in that band would silently vanish when
  // downsampling. (The production 1:1 sim->output ratio is unaffected — the
  // pad absorbs the edges — this matters when output res < sim res.)
  xs.front().first = 0;
  ys.front().first = 0;
  xs.back().second = src.width - 1;
  ys.back().second = src.height - 1;
  for (int y = 0; y < dst_res; ++y) {
    const auto [sy0, sy1] = ys[static_cast<size_t>(y)];
    for (int x = 0; x < dst_res; ++x) {
      const auto [sx0, sx1] = xs[static_cast<size_t>(x)];
      float m = 0.0f;
      for (int sy = sy0; sy <= sy1; ++sy)
        for (int sx = sx0; sx <= sx1; ++sx) m = std::max(m, src.at(sx, sy));
      out.at(x, y) = m;
    }
  }
  return out;
}

}  // namespace badlands::mapgen
