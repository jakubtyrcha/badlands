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

float micro_fill(Field2D<float>& B, Field2D<float>& S,
                 const Field2D<uint8_t>& basin_mask, float texel_m) {
  const int w = B.width, ht = B.height;
  Field2D<float> h(w, ht);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  const FlowRouting r = route_flow(h, texel_m, kEpsilonM);
  const float texel_area = texel_m * texel_m;

  double total_filled = 0.0;
  std::vector<uint8_t> seen(h.size(), 0);
  std::vector<int> stack, member;
  for (int start = 0; start < w * ht; ++start) {
    if (seen[start] || !r.in_lake[start]) continue;
    stack.assign(1, start);
    member.clear();
    seen[start] = 1;
    float max_depth = 0.0f;
    bool touches_basin = false;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      member.push_back(i);
      max_depth = std::max(max_depth, r.water_level[i] - h.data[i]);
      if (basin_mask.data[i]) touches_basin = true;
      const int x = i % w, y = i / w;
      const int nb[4] = {i - 1, i + 1, i - w, i + w};
      const bool ok[4] = {x > 0, x < w - 1, y > 0, y < ht - 1};
      for (int k = 0; k < 4; ++k)
        if (ok[k] && !seen[nb[k]] && r.in_lake[nb[k]]) {
          seen[nb[k]] = 1;
          stack.push_back(nb[k]);
        }
    }
    if (max_depth > kMicroFillCapM || touches_basin) continue;
    for (const int i : member) {
      const float fill = r.water_level[i] - h.data[i];
      S.data[i] += fill;
      total_filled += static_cast<double>(fill) * texel_area;
    }
  }
  return static_cast<float>(total_filled);
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

float deposit(Field2D<float>& B, Field2D<float>& S,
              const Field2D<float>& eroded_m, const FlowRouting& r,
              const Field2D<float>& area, const ErosionParams& p,
              float texel_area_m2) {
  std::vector<double> q_in(eroded_m.size(), 0.0);  // m³ arriving from donors
  double exported = 0.0;
  for (size_t k = r.order.size(); k-- > 0;) {  // donors before receivers
    const int i = r.order[k];
    double dep_depth = 0.0;
    if (q_in[i] > 0.0) {
      const double avail_depth = q_in[i] / texel_area_m2;
      if (r.in_lake[i]) {
        const double headroom = r.water_level[i] - (B.data[i] + S.data[i]);
        dep_depth = std::clamp(avail_depth, 0.0, std::max(0.0, headroom));
      } else {
        dep_depth = std::min(avail_depth,
                             static_cast<double>(p.deposition_g) * q_in[i] /
                                 std::max(area.data[i], texel_area_m2));
      }
      S.data[i] += static_cast<float>(dep_depth);
    }
    const double q_out =
        q_in[i] - dep_depth * texel_area_m2 + eroded_m.data[i] * texel_area_m2;
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0) q_in[rcv] += q_out;
    else exported += q_out;
  }
  return static_cast<float>(exported);
}

void diffuse(Field2D<float>& B, Field2D<float>& S, const ErosionParams& p,
             float texel_m) {
  if (p.diffusion <= 0.0f) return;
  const int w = B.width, ht = B.height;
  const float tex2 = texel_m * texel_m;
  const int n_sub = std::max(
      1, static_cast<int>(std::ceil(p.diffusion * p.dt / (0.24f * tex2))));
  const float dt_sub = p.dt / static_cast<float>(n_sub);
  Field2D<float> h(w, ht);
  for (int step = 0; step < n_sub; ++step) {
    for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
    for (int y = 1; y < ht - 1; ++y) {
      for (int x = 1; x < w - 1; ++x) {
        const float lap = h.at(x + 1, y) + h.at(x - 1, y) + h.at(x, y + 1) +
                          h.at(x, y - 1) - 4.0f * h.at(x, y);
        const float dh = p.diffusion * dt_sub * lap / tex2;
        if (dh >= 0.0f) {
          S.at(x, y) += dh;
        } else {
          const float from_s = std::min(S.at(x, y), -dh);
          S.at(x, y) -= from_s;
          B.at(x, y) -= (-dh - from_s);
        }
      }
    }
  }
}

namespace {

// Flood the CURRENT surface and turn flooded cells into water depths, then
// prune lakes (4-connected components) below the area/depth thresholds.
Field2D<float> finalize_lakes(const Field2D<float>& B, const Field2D<float>& S,
                              const FlowRouting& r, const ErosionParams& p,
                              float texel_m) {
  const int w = r.width, ht = r.height;
  Field2D<float> depth(w, ht, 0.0f);
  for (int i = 0; i < w * ht; ++i)
    if (r.in_lake[i])
      depth.data[i] = r.water_level[i] - (B.data[i] + S.data[i]);
  // component label + prune
  const float texel_area = texel_m * texel_m;
  std::vector<uint8_t> seen(depth.size(), 0);
  std::vector<int> stack, member;
  for (int start = 0; start < w * ht; ++start) {
    if (seen[start] || depth.data[start] <= 0.0f) continue;
    stack.assign(1, start);
    member.clear();
    seen[start] = 1;
    float max_depth = 0.0f;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      member.push_back(i);
      max_depth = std::max(max_depth, depth.data[i]);
      const int x = i % w, y = i / w;
      const int nb[4] = {i - 1, i + 1, i - w, i + w};
      const bool ok[4] = {x > 0, x < w - 1, y > 0, y < ht - 1};
      for (int k = 0; k < 4; ++k)
        if (ok[k] && !seen[nb[k]] && depth.data[nb[k]] > 0.0f) {
          seen[nb[k]] = 1;
          stack.push_back(nb[k]);
        }
    }
    const float area = static_cast<float>(member.size()) * texel_area;
    if (area < p.min_lake_area_m2 || max_depth < p.min_lake_depth_m)
      for (const int i : member) depth.data[i] = 0.0f;
  }
  return depth;
}

}  // namespace

ErosionOutputs erode(Field2D<float>& B, Field2D<float>& S,
                     const ErosionParams& p, float texel_m,
                     MapDebugSink* sink) {
  const float texel_area = texel_m * texel_m;
  Field2D<float> h(B.width, B.height);
  auto ground = [&] {
    for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  };
  FlowRouting r;
  Field2D<float> area;
  for (int it = 1; it <= p.iterations; ++it) {
    ground();
    r = route_flow(h, texel_m, kEpsilonM);
    area = accumulate_drainage(r, texel_area);
    const auto eroded = incise(B, S, r, area, p, texel_m);
    deposit(B, S, eroded, r, area, p, texel_area);
    diffuse(B, S, p, texel_m);
    if (sink && p.dump_every > 0 && it % p.dump_every == 0) {
      ground();
      sink->dump("loop-height", it, h);
      sink->dump("loop-flow", it, area);
      sink->dump("loop-sediment", it, S);
      Field2D<uint8_t> lakes(r.width, r.height, 0);
      for (size_t i = 0; i < lakes.data.size(); ++i) lakes.data[i] = r.in_lake[i];
      sink->dump("loop-lakes", it, lakes);
    }
  }
  ground();
  r = route_flow(h, texel_m, kEpsilonM);
  ErosionOutputs out;
  out.flow = accumulate_drainage(r, texel_area);
  out.water_depth = finalize_lakes(B, S, r, p, texel_m);
  return out;
}

}  // namespace badlands::mapgen
