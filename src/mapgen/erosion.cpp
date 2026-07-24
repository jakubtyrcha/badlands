#include "mapgen/erosion.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <FastNoiseLite.h>

namespace badlands::mapgen {

Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float lake_depth_m) {
  Field2D<uint8_t> mask(bedrock.width, bedrock.height, 0);
  const size_t n = bedrock.size();
  if (n == 0 || lake_frac <= 0.0f) return mask;
  const float frac = std::min(lake_frac, 1.0f);
  std::vector<float> v = bedrock.data;
  const size_t i_lake = static_cast<size_t>(frac * (n - 1));
  std::nth_element(v.begin(), v.begin() + i_lake, v.end());
  const float t_lake = v[i_lake];
  const float b_min = *std::min_element(bedrock.data.begin(), bedrock.data.end());
  const float span = std::max(t_lake - b_min, 1e-6f);
  for (size_t i = 0; i < n; ++i) {
    const float b = bedrock.data[i];
    if (b >= t_lake) continue;
    mask.data[i] = 1;
    const float u = (t_lake - b) / span;  // 0 at rim, 1 at the minimum
    B.data[i] -= lake_depth_m * u * u;    // smooth bowl: flat rim, deep center
  }
  return mask;
}

Field2D<float> init_sediment(const Field2D<float>& dist_to_plains,
                             const Field2D<uint8_t>& basin_mask,
                             const ErosionParams& p, float texel_m,
                             float origin_m, uint32_t seed) {
  Field2D<float> s(dist_to_plains.width, dist_to_plains.height, 0.0f);
  FastNoiseLite noise(static_cast<int>(seed + 3u));
  noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  noise.SetFractalType(FastNoiseLite::FractalType_FBm);
  noise.SetFractalOctaves(3);
  noise.SetFrequency(1.0f / p.sediment_noise_wavelength_m);
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      if (basin_mask.at(x, y)) continue;  // cavities start sediment-free
      const float taper =
          std::clamp(1.0f - dist_to_plains.at(x, y) / p.sediment_taper_m, 0.0f, 1.0f);
      const float wx = static_cast<float>(x) * texel_m + origin_m;
      const float wy = static_cast<float>(y) * texel_m + origin_m;
      const float nse = p.sediment_noise_m * noise.GetNoise(wx, wy);  // ~[-a, a]
      s.at(x, y) = std::max(0.0f, p.initial_sediment_m * taper + nse);
    }
  }
  return s;
}

Field2D<float> incise(Field2D<float>& B, Field2D<float>& S,
                      const FlowRouting& r, const Field2D<float>& area,
                      const ErosionParams& p, float texel_m) {
  Field2D<float> eroded(r.width, r.height, 0.0f);
  const float diag = texel_m * std::sqrt(2.0f);
  for (const int i : r.order) {
    const int32_t rcv = r.receiver[i];
    if (rcv < 0 || r.in_lake[i]) continue;  // base level / lake floor: skip
    const float h_old = B.data[i] + S.data[i];
    // effective receiver level: erode toward the water SURFACE over flooded
    // receivers (in_lake); chain through the in-sweep-updated ground
    // everywhere else (Braun–Willett: walk r.order so the receiver is
    // already updated).
    const float z_rcv = r.in_lake[rcv]
                             ? std::max(B.data[rcv] + S.data[rcv], r.water_level[rcv])
                             : B.data[rcv] + S.data[rcv];
    if (h_old <= z_rcv) continue;
    const int dx = std::abs(i % r.width - rcv % r.width);
    const int dy = std::abs(i / r.width - rcv / r.width);
    const float d = (dx + dy == 2) ? diag : texel_m;
    const bool on_sediment = S.data[i] > 0.0f;
    const float K = on_sediment ? p.k_sediment : p.k_bedrock;
    if (K <= 0.0f) continue;
    const float F = K * std::pow(area.data[i], p.m) * p.dt / d;
    const float h_new = (h_old + F * z_rcv) / (1.0f + F);
    float delta = h_old - h_new;
    if (delta <= 0.0f) continue;
    if (!on_sediment) {
      B.data[i] -= delta;  // pure bedrock step: delta is already at bedrock rate
    } else if (delta <= S.data[i]) {
      S.data[i] -= delta;
    } else {
      // mid-step transition: the sediment fraction went at k_sediment's rate,
      // the remainder is re-costed at bedrock's slower rate
      const float into_rock = (delta - S.data[i]) * (p.k_bedrock / p.k_sediment);
      delta = S.data[i] + into_rock;
      B.data[i] -= into_rock;
      S.data[i] = 0.0f;
    }
    eroded.data[i] = delta;
  }
  return eroded;
}

}  // namespace badlands::mapgen
