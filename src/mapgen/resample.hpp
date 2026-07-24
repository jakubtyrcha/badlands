#pragma once

// World-aligned bilinear resampling: reads a source Field2D at the
// destination grid's world positions (not texel-index positions), so a
// different resolution or origin resamples correctly. Used to go from the
// (possibly padded/offset) erosion sim grid to the output heightmap grid.

#include <algorithm>
#include <cmath>
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

}  // namespace badlands::mapgen
