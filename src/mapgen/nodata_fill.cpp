#include "mapgen/nodata_fill.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace badlands::mapgen {

namespace {

bool is_invalid(float v, float sentinel) {
  return v == sentinel || !std::isfinite(v);
}

}  // namespace

Field2D<uint8_t> fill_nodata(Field2D<float>& field, float sentinel) {
  const int w = field.width, h = field.height;
  Field2D<uint8_t> mask(w, h, 0);
  if (w <= 0 || h <= 0) return mask;

  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

  // Seed the frontier with every VALID texel, then flood outward. An index
  // vector walked by a head cursor rather than std::queue: the total work is
  // bounded by n, so one reserve covers the whole traversal.
  std::vector<int32_t> queue;
  queue.reserve(n);
  size_t invalid_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (is_invalid(field.data[i], sentinel)) {
      mask.data[i] = 1;
      ++invalid_count;
    } else {
      queue.push_back(static_cast<int32_t>(i));
    }
  }

  // Nothing to do, or nothing to fill FROM. Both leave the field untouched; the
  // all-invalid case keeps its full mask so the caller can see it.
  if (invalid_count == 0 || queue.empty()) return mask;

  for (size_t head = 0; head < queue.size(); ++head) {
    const int32_t cur = queue[head];
    const int cx = cur % w;
    const int cy = cur / w;
    const float value = field.data[static_cast<size_t>(cur)];

    constexpr int kDx[4] = {-1, 1, 0, 0};
    constexpr int kDy[4] = {0, 0, -1, 1};
    for (int k = 0; k < 4; ++k) {
      const int nx = cx + kDx[k];
      const int ny = cy + kDy[k];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      const size_t ni = static_cast<size_t>(ny) * w + nx;

      // The FIELD is the visited set, not the mask -- writing a valid value
      // into a texel is exactly what makes it stop qualifying, so a texel
      // cannot be claimed twice. Using the mask for this instead would destroy
      // the answer it is there to carry.
      if (mask.data[ni] == 0) continue;                 // was valid all along
      if (!is_invalid(field.data[ni], sentinel)) continue;  // already claimed
      field.data[ni] = value;
      queue.push_back(static_cast<int32_t>(ni));
    }
  }

  return mask;
}

}  // namespace badlands::mapgen
