#include "mapgen/fog_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "mapgen/biomes.hpp"

namespace badlands::mapgen {

namespace {

// Deterministic 32-bit integer hash (Murmur-style finalizer); jitters a patch's
// sample point reproducibly from (patch_x, patch_z, seed) with no global RNG.
uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
uint32_t Hash3(int px, int pz, uint32_t seed) {
  return Hash(static_cast<uint32_t>(px) * 0x9e3779b1u ^
              Hash(static_cast<uint32_t>(pz) * 0x85ebca77u ^ Hash(seed)));
}
float Unit(uint32_t h) { return static_cast<float>(h & 0xffffffu) / 16777216.0f; }

// Nearest height at world (wx, wz) (1 sample = 1 m), clamped to the field. A
// coarse base_y is all an emitter needs, so no bilinear filtering.
float HeightAt(const Field2D<float>& h, float wx, float wz) {
  if (h.width <= 0 || h.height <= 0) return 0.0f;
  const int x = std::clamp(static_cast<int>(wx), 0, h.width - 1);
  const int z = std::clamp(static_cast<int>(wz), 0, h.height - 1);
  return h.at(x, z);
}

// Eigen-decomposition of a 2x2 symmetric covariance [[a,b],[b,c]]:
// larger/smaller eigenvalues and the principal axis angle (yaw about Y).
void EigenSym2(float a, float b, float c, float& l0, float& l1, float& angle) {
  const float tr = 0.5f * (a + c);
  const float diff = 0.5f * (a - c);
  const float r = std::sqrt(diff * diff + b * b);
  l0 = tr + r;  // major
  l1 = tr - r;  // minor
  if (std::abs(b) > 1e-6f || std::abs(diff) > 1e-6f) {
    // Eigenvector of the major eigenvalue.
    angle = std::atan2(l0 - a, b);  // atan2 of (b, l0-c) equivalently; stable form
    // The above uses [[a,b],[b,c]]: (l0-a, b) is proportional to the eigenvector
    // when b != 0; fall back handled by the guard.
  } else {
    angle = 0.0f;  // isotropic: orientation is arbitrary
  }
}

}  // namespace

std::vector<fog::Emitter> GenerateBiomeFog(const Field2D<uint8_t>& biome,
                                           const Field2D<float>& height,
                                           uint32_t seed,
                                           const BiomeFogParams& params) {
  std::vector<fog::Emitter> out;
  if (biome.width <= 0 || biome.height <= 0) return out;

  const int W = biome.width, H = biome.height;
  const float patch = params.patch_m;
  const float radius = params.gather_radius_m;
  const int ri = static_cast<int>(std::ceil(radius));
  const int patches_x = static_cast<int>(std::ceil(W / patch));
  const int patches_z = static_cast<int>(std::ceil(H / patch));

  const uint8_t kForest = static_cast<uint8_t>(Biome::Forest);
  const uint8_t kSwamp = static_cast<uint8_t>(Biome::Swamp);

  for (int pz = 0; pz < patches_z; ++pz) {
    for (int px = 0; px < patches_x; ++px) {
      // Jittered sample point inside the patch (deterministic per patch+seed).
      const uint32_t h0 = Hash3(px, pz, seed);
      const uint32_t h1 = Hash3(px, pz, seed ^ 0x68bc21ebu);
      const float sx = (px + Unit(h0)) * patch;
      const float sz = (pz + Unit(h1)) * patch;
      const int cx = static_cast<int>(sx), cz = static_cast<int>(sz);
      if (cx < 0 || cz < 0 || cx >= W || cz >= H) continue;

      // Vote + covariance accumulation over the disc of radius `radius`.
      std::array<int, kBiomeCount> votes{};
      const float r2 = radius * radius;
      for (int dz = -ri; dz <= ri; ++dz) {
        const int z = cz + dz;
        if (z < 0 || z >= H) continue;
        for (int dx = -ri; dx <= ri; ++dx) {
          const int x = cx + dx;
          if (x < 0 || x >= W) continue;
          if (dx * dx + dz * dz > r2) continue;
          const uint8_t b = biome.at(x, z);
          if (b < kBiomeCount) ++votes[b];
        }
      }
      // Majority biome; only Forest/Swamp qualify.
      uint8_t majority = 0;
      int best = -1;
      for (int i = 0; i < kBiomeCount; ++i)
        if (votes[i] > best) { best = votes[i]; majority = static_cast<uint8_t>(i); }
      if (majority != kForest && majority != kSwamp) continue;
      if (votes[majority] < params.min_samples) continue;

      // Covariance of the matching-biome sample positions in the disc.
      double n = 0, mx = 0, mz = 0, sxx = 0, szz = 0, sxz = 0;
      for (int dz = -ri; dz <= ri; ++dz) {
        const int z = cz + dz;
        if (z < 0 || z >= H) continue;
        for (int dx = -ri; dx <= ri; ++dx) {
          const int x = cx + dx;
          if (x < 0 || x >= W) continue;
          if (dx * dx + dz * dz > r2) continue;
          if (biome.at(x, z) != majority) continue;
          const double wx = x + 0.5, wz = z + 0.5;
          n += 1; mx += wx; mz += wz; sxx += wx * wx; szz += wz * wz; sxz += wx * wz;
        }
      }
      if (n < params.min_samples) continue;
      mx /= n; mz /= n;
      const float cxx = static_cast<float>(sxx / n - mx * mx);
      const float czz = static_cast<float>(szz / n - mz * mz);
      const float cxz = static_cast<float>(sxz / n - mx * mz);

      float l0, l1, angle;
      EigenSym2(cxx, cxz, czz, l0, l1, angle);
      const float hx = std::max(params.min_extent_m,
                                params.extent_scale * std::sqrt(std::max(l0, 0.0f)));
      const float hz = std::max(params.min_extent_m,
                                params.extent_scale * std::sqrt(std::max(l1, 0.0f)));

      fog::Emitter e;
      e.center = {static_cast<float>(mx), static_cast<float>(mz)};
      e.half_extent = {hx, hz};
      e.rotation = angle;
      e.shape = fog::EmitterShape::Ellipse;  // OBB frame, radial density
      e.base_y = HeightAt(height, e.center.x, e.center.y) + params.base_lift_m;
      e.height = params.height_m;
      e.magnitude = params.magnitude;
      e.radial_falloff = params.radial_falloff;
      e.vertical_falloff = params.vertical_falloff;
      if (majority == kSwamp) {
        // Granular, drifting fog: the same elliptical footprint × a time-animated
        // noise slice.
        e.type = fog::EmitterType::Noise;
        e.noise_freq = params.noise_freq;
        e.noise_contrast = params.noise_contrast;
        e.scroll = params.noise_scroll;
        e.seed = Unit(Hash3(px, pz, seed ^ 0x1b56c4e9u)) * 100.0f;
      } else {
        e.type = fog::EmitterType::Disc;  // flat radial fog
      }
      out.push_back(e);
    }
  }
  return out;
}

std::vector<fog::Emitter> BuildBorderFog(glm::vec2 map_min, glm::vec2 map_max,
                                         const BorderFogParams& params) {
  std::vector<fog::Emitter> out;
  // Map corners in world XZ (glm::vec2 = (x, z)), CCW.
  const glm::vec2 corner[4] = {{map_min.x, map_min.y},
                               {map_max.x, map_min.y},
                               {map_max.x, map_max.y},
                               {map_min.x, map_max.y}};
  const float band = params.band_m;
  for (int i = 0; i < 4; ++i) {
    const glm::vec2 a = corner[i], b = corner[(i + 1) % 4];
    glm::vec2 dir = b - a;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-3f) continue;
    dir /= len;

    fog::Emitter e;
    e.center = 0.5f * (a + b);              // on the edge line
    e.rotation = std::atan2(dir.y, dir.x);  // local x along the edge
    e.half_extent = {0.5f * len + band, band};  // along edge (padded), perp band
    e.shape = fog::EmitterShape::Obb;
    e.type = fog::EmitterType::Disc;  // flat milk-white
    e.base_y = 0.0f;
    e.height = params.height_m;
    e.magnitude = params.magnitude;
    e.radial_falloff = std::clamp(params.ramp_m / std::max(band, 1e-3f), 0.0f, 1.0f);
    e.vertical_falloff = 0.3f;
    out.push_back(e);
  }
  return out;
}

}  // namespace badlands::mapgen
