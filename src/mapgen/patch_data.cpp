#include "mapgen/patch_data.hpp"

#include <algorithm>
#include <limits>

namespace badlands::mapgen {

ElevationRange compute_elevation_range(const Field2D<float>& height) {
  if (height.data.empty()) return {};
  float lo = std::numeric_limits<float>::infinity();
  float hi = -std::numeric_limits<float>::infinity();
  for (float h : height.data) {
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  }
  // A raster of non-finite samples would leave the sentinels in place; clamp to
  // a degenerate-but-usable range rather than handing the camera an infinity.
  if (!(lo <= hi)) return {};
  return {lo, hi};
}

}  // namespace badlands::mapgen
