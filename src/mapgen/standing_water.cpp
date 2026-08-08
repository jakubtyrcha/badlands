#include "mapgen/standing_water.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "mapgen/cover.hpp"

namespace badlands::mapgen {

namespace {

constexpr int kDx[4] = {-1, 1, 0, 0};
constexpr int kDy[4] = {0, 0, -1, 1};

// Median rather than mean. The plate is flat to within a few centimetres, but
// the 10 m cover mask over-claims at every edge and picks up a handful of bank
// texels metres higher (measured at 253.37..261.87 m around a 253.43 m
// surface); a mean would ride up with them. A median over a flat plate is the
// plate.
//
// The median is used ONLY to pick the surface elevation. It emphatically does
// not filter the extent: half a plate lies above its own median by definition,
// so rejecting those texels once shredded a single lake into ten fragments and
// lost a quarter of its area.
float median_of(std::vector<float>& v) {
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  return v[mid];
}

}  // namespace

StandingWater derive_standing_water(const Field2D<float>& dtm,
                                    const Field2D<uint8_t>& cover,
                                    float texel_m) {
  StandingWater out;
  const int w = dtm.width, h = dtm.height;
  if (w <= 0 || h <= 0 || texel_m <= 0.0f) return out;

  // Dry everywhere is the honest default: water nothing observed is water there
  // is no evidence for.
  out.bed = dtm;
  out.level = dtm;
  if (cover.width != w || cover.height != h) return out;

  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  const auto water_class = static_cast<uint8_t>(Cover::Water);

  auto is_water = [&](size_t i) { return cover.data[i] == water_class; };

  // --- 1. Distance to the nearest non-water texel, in texels ------------------
  //
  // One multi-source BFS over the whole raster, seeded from every DRY texel, not
  // one traversal per lake. That is both simpler and the literal definition of
  // the quantity the bed model wants.
  //
  // The patch edge is deliberately NOT seeded: water leaving the frame carries
  // on in the real world, so tapering it to zero against the boundary would
  // invent a shore.
  constexpr int32_t kUnreached = -1;
  std::vector<int32_t> dist(n, kUnreached);
  std::vector<int32_t> queue;
  queue.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!is_water(i)) {
      dist[i] = 0;
      queue.push_back(static_cast<int32_t>(i));
    }
  }
  for (size_t head = 0; head < queue.size(); ++head) {
    const int32_t cur = queue[head];
    const int cx = cur % w, cy = cur / w;
    for (int k = 0; k < 4; ++k) {
      const int nx = cx + kDx[k], ny = cy + kDy[k];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      const size_t ni = static_cast<size_t>(ny) * w + nx;
      if (dist[ni] != kUnreached) continue;
      dist[ni] = dist[static_cast<size_t>(cur)] + 1;
      queue.push_back(static_cast<int32_t>(ni));
    }
  }

  // --- 2. One surface elevation per connected body ----------------------------
  std::vector<uint8_t> seen(n, 0);
  std::vector<int32_t> stack, members;
  std::vector<float> samples;

  for (size_t seed = 0; seed < n; ++seed) {
    if (!is_water(seed) || seen[seed]) continue;

    members.clear();
    samples.clear();
    stack.assign(1, static_cast<int32_t>(seed));
    seen[seed] = 1;
    while (!stack.empty()) {
      const int32_t cur = stack.back();
      stack.pop_back();
      members.push_back(cur);
      samples.push_back(dtm.data[static_cast<size_t>(cur)]);

      const int cx = cur % w, cy = cur / w;
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + kDx[k], ny = cy + kDy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const size_t ni = static_cast<size_t>(ny) * w + nx;
        if (!is_water(ni) || seen[ni]) continue;
        seen[ni] = 1;
        stack.push_back(static_cast<int32_t>(ni));
      }
    }

    // EVERY member is water. The extent is the observed mask, full stop -- no
    // filter by elevation, no growth into ground that merely looks low enough.
    const float surface = median_of(samples);

    // --- 3. Carve --------------------------------------------------------------
    for (int32_t idx : members) {
      const size_t i = static_cast<size_t>(idx);
      // Unreached means the whole raster is water: no shore anywhere, so the
      // basin is at its full depth rather than at zero.
      const float shore_m =
          dist[i] == kUnreached
              ? kMaxLakeDepthM / std::max(kLakeBedSlope, 1e-6f)
              : static_cast<float>(dist[i]) * texel_m;
      const float depth = std::min(kMaxLakeDepthM, kLakeBedSlope * shore_m);
      out.level.data[i] = surface;
      // Never RAISE the bed: ground already below the modelled depth is real
      // survey data (a dredged channel, a quarry pool) and outranks the model.
      out.bed.data[i] = std::min(out.bed.data[i], surface - depth);
    }
  }

  return out;
}

}  // namespace badlands::mapgen
