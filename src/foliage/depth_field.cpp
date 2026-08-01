#include "foliage/depth_field.hpp"

#include <algorithm>
#include <cmath>

#include <FastNoiseLite.h>

#include "mapgen/distance_field.hpp"

namespace badlands::foliage {

namespace {

int RasterExtent(float size_m, float texel_m) {
  if (size_m <= 0.0f || texel_m <= 0.0f) return 0;
  // One more texel than spans, so the raster covers the closed rect: texel i
  // sits at i * texel_m and the last one lands on (or just past) the far edge.
  return static_cast<int>(std::ceil(size_m / texel_m)) + 1;
}

}  // namespace

float DepthField::DepthAt(float x, float z) const {
  if (empty()) return 0.0f;

  const float fx = (x - origin_m.x) / texel_m;
  const float fz = (z - origin_m.y) / texel_m;
  const float cx = std::clamp(fx, 0.0f, static_cast<float>(width - 1));
  const float cz = std::clamp(fz, 0.0f, static_cast<float>(height - 1));

  const int x0 = static_cast<int>(cx);
  const int z0 = static_cast<int>(cz);
  const int x1 = std::min(x0 + 1, width - 1);
  const int z1 = std::min(z0 + 1, height - 1);
  const float tx = cx - static_cast<float>(x0);
  const float tz = cz - static_cast<float>(z0);

  const float d00 = depth[static_cast<size_t>(z0) * width + x0];
  const float d10 = depth[static_cast<size_t>(z0) * width + x1];
  const float d01 = depth[static_cast<size_t>(z1) * width + x0];
  const float d11 = depth[static_cast<size_t>(z1) * width + x1];

  const float a = d00 + (d10 - d00) * tx;
  const float b = d01 + (d11 - d01) * tx;
  return a + (b - a) * tz;
}

DepthField BuildDepthField(const TerrainQuery& query, glm::vec2 origin_m,
                           glm::vec2 size_m, float texel_m,
                           const ForestNoise& noise, uint32_t seed) {
  DepthField out;
  out.origin_m = origin_m;
  out.texel_m = texel_m;
  out.width = RasterExtent(size_m.x, texel_m);
  out.height = RasterExtent(size_m.y, texel_m);
  if (out.empty()) return {};

  // The mask the EDT measures FROM is the forest's COMPLEMENT: distance to the
  // nearest non-forest texel is exactly depth-into-forest, and it is 0 outside
  // the forest (where the texel is its own nearest seed) with no extra work.
  mapgen::Field2D<uint8_t> outside(out.width, out.height, 0);
  bool any_outside = false;
  for (int j = 0; j < out.height; ++j) {
    for (int i = 0; i < out.width; ++i) {
      const float wx = origin_m.x + static_cast<float>(i) * texel_m;
      const float wz = origin_m.y + static_cast<float>(j) * texel_m;
      const bool inside = query.CoverageAt(wx, wz) >= kCoverageMaskThreshold;
      if (!inside) {
        outside.at(i, j) = 1;
        any_outside = true;
      }
    }
  }

  out.depth.assign(static_cast<size_t>(out.width) * out.height, 0.0f);

  if (!any_outside) {
    // Every texel is forest, so there is no edge on this map to measure from.
    // distance_to_mask would return all zeros here (its documented no-seed
    // degenerate), which would read as "the entire map is a forest edge" and
    // suppress the canopy completely. Deep interior is the right answer.
    std::fill(out.depth.begin(), out.depth.end(), kInteriorDepthM);
  } else {
    const mapgen::Field2D<float> edt =
        mapgen::distance_to_mask(outside, glm::vec2(texel_m, texel_m));
    std::copy(edt.data.begin(), edt.data.end(), out.depth.begin());
  }

  if (noise.warp_amp_m > 0.0f && noise.warp_wavelength_m > 0.0f) {
    FastNoiseLite warp;
    warp.SetSeed(static_cast<int>(seed ^ 0x5eed1234u));
    warp.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    warp.SetFrequency(1.0f / noise.warp_wavelength_m);
    for (int j = 0; j < out.height; ++j) {
      for (int i = 0; i < out.width; ++i) {
        const float wx = origin_m.x + static_cast<float>(i) * texel_m;
        const float wz = origin_m.y + static_cast<float>(j) * texel_m;
        float& d = out.depth[static_cast<size_t>(j) * out.width + i];
        // FastNoiseLite returns ~[-1, 1], so this pushes the boundary both
        // outward and inward -- a one-sided warp would just dilate the forest.
        d = std::max(0.0f, d + noise.warp_amp_m * warp.GetNoise(wx, wz));
      }
    }
  }

  return out;
}

}  // namespace badlands::foliage
