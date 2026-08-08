#include "mapgen/standing_water.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "mapgen/cover.hpp"

namespace badlands::mapgen {

namespace {

constexpr int kDx[4] = {-1, 1, 0, 0};
constexpr int kDy[4] = {0, 0, -1, 1};

// Owner sentinels. Distinguishing "never looked at" from "looked at and
// rejected" is what stops a rejected component being re-seeded once per member.
constexpr int32_t kUnassigned = -1;
constexpr int32_t kRejected = -2;

// Median rather than mean: the plate is flat to within a few centimetres, but
// the mask's edge catches a handful of bank texels metres higher (measured at
// 253.37..261.87 m around a 253.43 m surface), and a mean would ride up with
// them. A median over a flat plate is the plate.
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

  // Dry everywhere is the honest default: a lake nothing observed is a lake
  // there is no evidence for.
  out.bed = dtm;
  out.level = dtm;
  if (cover.width != w || cover.height != h) return out;

  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  const auto water_class = static_cast<uint8_t>(Cover::Water);

  std::vector<int32_t> owner(n, kUnassigned);
  // Member slot + 1 for the lake currently being carved, 0 otherwise. Hoisted
  // out of the loop and cleared only where it was written -- allocating a
  // patch-sized vector per lake would dominate everything else here.
  std::vector<int32_t> slot(n, 0);
  std::vector<int32_t> stack, members, frontier, dist;
  std::vector<float> samples;

  for (size_t seed = 0; seed < n; ++seed) {
    if (cover.data[seed] != water_class || owner[seed] != kUnassigned) continue;

    // 1. The observed component. An explicit stack, not recursion: a lake can
    //    span the whole patch.
    const int32_t lake = 0;  // identity is not needed past this iteration
    members.clear();
    samples.clear();
    stack.assign(1, static_cast<int32_t>(seed));
    owner[seed] = lake;
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
        if (cover.data[ni] != water_class || owner[ni] != kUnassigned) continue;
        owner[ni] = lake;
        stack.push_back(static_cast<int32_t>(ni));
      }
    }

    const float surface = median_of(samples);

    // Land cover is 10 m against 1 m relief, so the observed mask over-claims
    // at every edge -- it takes in banks, spits and causeways that the survey
    // measured ABOVE the water surface. Carving those down would dig a hole
    // where there is visibly ground, so they leave the lake here, exactly as
    // the growth step below refuses to take in anything above the surface.
    // Both directions of the shoreline are then decided by the bed.
    std::erase_if(members, [&](int32_t idx) {
      const size_t i = static_cast<size_t>(idx);
      if (dtm.data[i] <= surface) return false;
      owner[i] = kRejected;
      return true;
    });

    if (static_cast<int>(members.size()) < kMinLakeTexels) {
      for (int32_t idx : members) owner[static_cast<size_t>(idx)] = kRejected;
      continue;
    }

    // 2. Grow to the true shoreline. The observed mask is 10 m land cover, so
    //    its edge is a staircase; the level cutting the real bed is not. Only
    //    ground genuinely BELOW the surface joins, so growth stops of its own
    //    accord at the rim (measured +1.56 m and +0.17 m above the plate).
    //    Rejected specks below the surface are absorbed, which is what they
    //    almost always are.
    stack.assign(members.begin(), members.end());
    while (!stack.empty()) {
      const int32_t cur = stack.back();
      stack.pop_back();
      const int cx = cur % w, cy = cur / w;
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + kDx[k], ny = cy + kDy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const size_t ni = static_cast<size_t>(ny) * w + nx;
        if (owner[ni] >= 0) continue;
        if (dtm.data[ni] >= surface) continue;
        owner[ni] = lake;
        members.push_back(static_cast<int32_t>(ni));
        stack.push_back(static_cast<int32_t>(ni));
      }
    }

    // 3. Distance from the shoreline: a multi-source BFS inward, seeded from
    //    every member touching non-member ground. The same traversal that
    //    defines the shore also drives the taper, so the two cannot disagree.
    for (size_t m = 0; m < members.size(); ++m) {
      slot[static_cast<size_t>(members[m])] = static_cast<int32_t>(m) + 1;
    }
    dist.assign(members.size(), -1);
    frontier.clear();
    for (size_t m = 0; m < members.size(); ++m) {
      const int32_t idx = members[m];
      const int cx = idx % w, cy = idx / w;
      bool on_shore = false;
      for (int k = 0; k < 4 && !on_shore; ++k) {
        const int nx = cx + kDx[k], ny = cy + kDy[k];
        // The patch edge is as much of a shore as anything else we can see.
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
          on_shore = true;
        } else if (slot[static_cast<size_t>(ny) * w + nx] == 0) {
          on_shore = true;
        }
      }
      if (on_shore) {
        dist[m] = 0;
        frontier.push_back(static_cast<int32_t>(m));
      }
    }
    for (size_t head = 0; head < frontier.size(); ++head) {
      const int32_t m = frontier[head];
      const int32_t idx = members[static_cast<size_t>(m)];
      const int cx = idx % w, cy = idx / w;
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + kDx[k], ny = cy + kDy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const int32_t s = slot[static_cast<size_t>(ny) * w + nx];
        if (s == 0 || dist[static_cast<size_t>(s - 1)] >= 0) continue;
        dist[static_cast<size_t>(s - 1)] = dist[static_cast<size_t>(m)] + 1;
        frontier.push_back(s - 1);
      }
    }

    // 4. Carve, then clear the scratch for the next lake.
    for (size_t m = 0; m < members.size(); ++m) {
      const size_t i = static_cast<size_t>(members[m]);
      const float shore_m = static_cast<float>(std::max(dist[m], 0)) * texel_m;
      const float depth =
          kAssumedLakeDepthM * std::min(1.0f, shore_m / kShoreTaperM);
      out.level.data[i] = surface;
      // Never RAISE the bed: ground already below the tapered depth is real
      // survey data (a channel, a dredged cut) and outranks the assumption.
      out.bed.data[i] = std::min(out.bed.data[i], surface - depth);
      slot[i] = 0;
    }
  }

  return out;
}

}  // namespace badlands::mapgen
