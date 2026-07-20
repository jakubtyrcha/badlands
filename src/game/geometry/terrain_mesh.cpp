#include "game/geometry/terrain_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/glm.hpp>

#include "mapgen/mapgen_constants.hpp"

namespace badlands {

namespace {

using mapgen::Field2D;
using mapgen::kMetersPerSample;

// All sampling below takes WORLD-meter coordinates and converts to heightmap
// sample space internally (world_m / kMetersPerSample), so the coordinate
// convention is centralized in one place and correct for any sample density.

}  // namespace

float SampleHeight(const Field2D<float>& h, float wx, float wz) {
  const float mps = static_cast<float>(kMetersPerSample);
  const float sx = std::clamp(wx / mps, 0.0f, static_cast<float>(h.width - 1));
  const float sz = std::clamp(wz / mps, 0.0f, static_cast<float>(h.height - 1));
  const int x0 = static_cast<int>(std::floor(sx));
  const int z0 = static_cast<int>(std::floor(sz));
  const int x1 = std::min(x0 + 1, h.width - 1);
  const int z1 = std::min(z0 + 1, h.height - 1);
  const float tx = sx - x0;
  const float tz = sz - z0;
  const float h00 = h.at(x0, z0), h10 = h.at(x1, z0);
  const float h01 = h.at(x0, z1), h11 = h.at(x1, z1);
  return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
}

glm::vec3 NormalAt(const Field2D<float>& h, float wx, float wz) {
  const float step = static_cast<float>(kMetersPerSample);
  const float hl = SampleHeight(h, wx - step, wz);
  const float hr = SampleHeight(h, wx + step, wz);
  const float hd = SampleHeight(h, wx, wz - step);
  const float hu = SampleHeight(h, wx, wz + step);
  const float d = 2.0f * step;
  return glm::normalize(glm::vec3(-(hr - hl) / d, 1.0f, -(hu - hd) / d));
}

float PackU8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint32_t u = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
                     (static_cast<uint32_t>(c) << 16) |
                     (static_cast<uint32_t>(d) << 24);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

bool RaycastTerrain(const Field2D<float>& heightmap, const Ray& ray,
                    glm::vec3& out_hit) {
  if (heightmap.width <= 0 || heightmap.height <= 0) return false;

  const float mps = static_cast<float>(kMetersPerSample);
  const float max_x = (heightmap.width - 1) * mps;
  const float max_z = (heightmap.height - 1) * mps;
  // Bound the march by the map diagonal + vertical span: past that the ray can
  // only be leaving.
  const float span = std::sqrt(max_x * max_x + max_z * max_z);
  const float kStep = mps;  // one sample per step -- can't skip a whole cell
  const float kMaxDist = span * 2.0f;

  auto inside_xz = [&](const glm::vec3& p) {
    return p.x >= 0.0f && p.z >= 0.0f && p.x <= max_x && p.z <= max_z;
  };
  // Signed height above the surface; negative once the ray is underground.
  auto above = [&](const glm::vec3& p) {
    return p.y - SampleHeight(heightmap, p.x, p.z);
  };

  glm::vec3 prev = ray.origin;
  // Starting underground (camera inside a hill): nothing sensible to pick.
  if (inside_xz(prev) && above(prev) < 0.0f) return false;

  for (float t = kStep; t <= kMaxDist; t += kStep) {
    const glm::vec3 cur = ray.At(t);
    const float cur_above = above(cur);

    if (inside_xz(cur) && cur_above <= 0.0f) {
      // Crossed the surface between prev and cur -- bisect for the crossing.
      glm::vec3 lo = prev, hi = cur;
      for (int i = 0; i < 24; ++i) {
        const glm::vec3 mid = (lo + hi) * 0.5f;
        if (above(mid) > 0.0f) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      out_hit = (lo + hi) * 0.5f;
      // Snap y onto the surface: bisection converges on t, and the ray may be
      // shallow enough that a small t error shows up in y.
      out_hit.y = SampleHeight(heightmap, out_hit.x, out_hit.z);
      return true;
    }
    prev = cur;
  }
  return false;
}

}  // namespace badlands
