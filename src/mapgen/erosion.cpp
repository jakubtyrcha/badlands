#include "mapgen/erosion.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <FastNoiseLite.h>

namespace badlands::mapgen {

// Forward-declared rather than including mapgen/generator.hpp: generator.hpp
// includes erosion.hpp, so the reverse include would cycle. distance_to_mask
// is defined in generator.cpp, which every target linking erosion.cpp also
// compiles (badlands_mapgen_lib, badlands_generator_tests,
// badlands_erosion_tests), so this resolves at link time.
Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask, glm::vec2 texel_m);

Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float slope_m_per_m,
                                glm::vec2 texel_m) {
  Field2D<uint8_t> mask(bedrock.width, bedrock.height, 0);
  const size_t n = bedrock.size();
  if (n == 0 || lake_frac <= 0.0f) return mask;
  const float frac = std::min(lake_frac, 1.0f);
  std::vector<float> v = bedrock.data;
  const size_t i_lake = static_cast<size_t>(frac * (n - 1));
  std::nth_element(v.begin(), v.begin() + i_lake, v.end());
  const float t_lake = v[i_lake];
  for (size_t i = 0; i < n; ++i) mask.data[i] = bedrock.data[i] < t_lake ? 1 : 0;

  // Invert: seeds are non-basin texels, so distance_to_mask gives each basin
  // texel its exact EDT distance to the nearest rim/dry cell (0 outside the
  // basin, since every non-basin cell is its own seed).
  Field2D<uint8_t> inverted(mask.width, mask.height);
  for (size_t i = 0; i < n; ++i) inverted.data[i] = mask.data[i] ? 0 : 1;
  const Field2D<float> dist = distance_to_mask(inverted, texel_m);

  for (size_t i = 0; i < n; ++i)
    if (mask.data[i]) B.data[i] -= slope_m_per_m * dist.data[i];
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

namespace {

// 4-connected components of cells for which `flag[i]` is truthy. Components
// are returned in the order their first (lowest-index) member is discovered
// by the scan — deterministic. Shared by micro_fill and deposit's lake pour.
std::vector<std::vector<int>> label_lake_components(int w, int ht,
                                                     const std::vector<uint8_t>& flag) {
  std::vector<std::vector<int>> components;
  std::vector<uint8_t> seen(flag.size(), 0);
  std::vector<int> stack;
  for (int start = 0; start < w * ht; ++start) {
    if (seen[start] || !flag[start]) continue;
    stack.assign(1, start);
    seen[start] = 1;
    std::vector<int> member;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      member.push_back(i);
      const int x = i % w, y = i / w;
      const int nb[4] = {i - 1, i + 1, i - w, i + w};
      const bool ok[4] = {x > 0, x < w - 1, y > 0, y < ht - 1};
      for (int k = 0; k < 4; ++k)
        if (ok[k] && !seen[nb[k]] && flag[nb[k]]) {
          seen[nb[k]] = 1;
          stack.push_back(nb[k]);
        }
    }
    components.push_back(std::move(member));
  }
  return components;
}

}  // namespace

float micro_fill(Field2D<float>& B, Field2D<float>& S,
                 const Field2D<uint8_t>& basin_mask, float texel_m) {
  const int w = B.width, ht = B.height;
  Field2D<float> h(w, ht);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  const FlowRouting r = route_flow(h, texel_m, kEpsilonM);
  const float texel_area = texel_m * texel_m;

  double total_filled = 0.0;
  for (const auto& member : label_lake_components(w, ht, r.in_lake)) {
    float max_depth = 0.0f;
    bool touches_basin = false;
    for (const int i : member) {
      max_depth = std::max(max_depth, r.water_level[i] - h.data[i]);
      if (basin_mask.data[i]) touches_basin = true;
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

namespace {

// Normal dry-cell deposition rule, applied to a volume `vol` (m³) arriving
// at `i`. Returns the volume actually deposited (m³); `vol` is left with
// whatever remains to forward.
double dry_deposit(int i, double vol, Field2D<float>& S, const Field2D<float>& area,
                   const ErosionParams& p, float texel_area_m2) {
  if (vol <= 0.0) return 0.0;
  const double avail_depth = vol / texel_area_m2;
  const double dep_depth =
      std::min(avail_depth, static_cast<double>(p.deposition_g) * vol /
                                 std::max(area.data[i], texel_area_m2));
  S.data[i] += static_cast<float>(dep_depth);
  return dep_depth * texel_area_m2;
}

}  // namespace

float deposit(Field2D<float>& B, Field2D<float>& S,
              const Field2D<float>& eroded_m, const FlowRouting& r,
              const Field2D<float>& area, const ErosionParams& p,
              float texel_area_m2) {
  const int w = r.width, ht = r.height;
  const size_t n = eroded_m.size();

  const auto components = label_lake_components(w, ht, r.in_lake);
  std::vector<int32_t> comp_of(n, -1);
  for (size_t c = 0; c < components.size(); ++c)
    for (const int i : components[c]) comp_of[i] = static_cast<int32_t>(c);

  // Position of each cell within r.order (its "pop order"); used both to
  // find the deepest — last-flooded — member of a component and to sort
  // components into a deterministic upstream-before-downstream processing
  // order (see kernel loop below).
  std::vector<int32_t> pop_index(n, -1);
  for (size_t k = 0; k < r.order.size(); ++k)
    pop_index[static_cast<size_t>(r.order[k])] = static_cast<int32_t>(k);

  // Follows a member's receiver chain until it leaves ITS OWN component
  // (a cell already outside `own_component` is returned immediately, be it
  // dry or a different lake — the caller's poured/unpoured check decides
  // what happens next; component labeling is 4-connected but the receiver
  // graph is 8-connected, so a component's escaping edge could in principle
  // land diagonally in a different lake rather than on dry ground). Every
  // component has exactly one such exit: priority-flood claims each
  // interior cell from exactly one already-visited neighbor, so a
  // component's internal receiver edges form a tree rooted at its single
  // spill point.
  auto find_exit = [&](int start, int32_t own_component) {
    int i = start;
    while (comp_of[i] == own_component) {
      const int32_t rcv = r.receiver[i];
      if (rcv < 0) break;  // guard: should not happen for a real lake member
      i = rcv;
    }
    return i;
  };

  std::vector<double> bucket(components.size(), 0.0);
  std::vector<uint8_t> poured(components.size(), 0);

  std::vector<double> q_in(n, 0.0);  // m³ arriving from donors
  double exported = 0.0;
  for (size_t k = r.order.size(); k-- > 0;) {  // donors before receivers
    const int i = r.order[k];
    const int32_t c = comp_of[i];
    if (c >= 0) {
      // In_lake: no local deposit, no forwarding along the internal chain —
      // the flux is bucketed for this cell's component and resolved by the
      // pour below.
      bucket[static_cast<size_t>(c)] +=
          q_in[static_cast<size_t>(i)] + static_cast<double>(eroded_m.data[i]) * texel_area_m2;
      continue;
    }
    const double dep = dry_deposit(i, q_in[static_cast<size_t>(i)], S, area, p, texel_area_m2);
    const double q_out =
        q_in[static_cast<size_t>(i)] - dep + eroded_m.data[i] * texel_area_m2;
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0) q_in[static_cast<size_t>(rcv)] += q_out;
    else exported += q_out;
  }

  // Deterministic processing order: descending pop-order of each
  // component's deepest (last-flooded) member. Flooding proceeds
  // border-inward, so a downstream lake's members are — generically —
  // claimed earlier than an upstream lake's; processing the higher
  // pop-order (upstream) component first guarantees its overflow finds the
  // downstream component still unpoured when it cascades into it.
  std::vector<int> proc_order(components.size());
  for (size_t c = 0; c < components.size(); ++c) proc_order[c] = static_cast<int>(c);
  std::sort(proc_order.begin(), proc_order.end(), [&](int a, int b) {
    int32_t deepest_a = components[static_cast<size_t>(a)][0];
    for (const int i : components[static_cast<size_t>(a)])
      if (pop_index[i] > pop_index[deepest_a]) deepest_a = i;
    int32_t deepest_b = components[static_cast<size_t>(b)][0];
    for (const int i : components[static_cast<size_t>(b)])
      if (pop_index[i] > pop_index[deepest_b]) deepest_b = i;
    return pop_index[deepest_a] > pop_index[deepest_b];
  });

  for (const int c : proc_order) {
    const auto& member = components[static_cast<size_t>(c)];
    // Sort bottom-up: lowest ground first, ties by linear index.
    std::vector<int> sorted_members = member;
    std::sort(sorted_members.begin(), sorted_members.end(), [&](int a, int b) {
      const float ha = B.data[a] + S.data[a];
      const float hb = B.data[b] + S.data[b];
      if (ha != hb) return ha < hb;
      return a < b;
    });
    double wl_cap = static_cast<double>(r.water_level[sorted_members[0]]);
    for (const int i : sorted_members)
      wl_cap = std::min(wl_cap, static_cast<double>(r.water_level[i]));

    double vol = bucket[static_cast<size_t>(c)];
    double level = static_cast<double>(B.data[sorted_members[0]] + S.data[sorted_members[0]]);
    size_t pool = 1;
    for (size_t k = 1; k <= sorted_members.size(); ++k) {
      const double next_boundary =
          (k < sorted_members.size())
              ? std::min(wl_cap, static_cast<double>(B.data[sorted_members[k]] +
                                                      S.data[sorted_members[k]]))
              : wl_cap;
      if (next_boundary > level) {
        const double need = (next_boundary - level) * static_cast<double>(pool) * texel_area_m2;
        if (vol >= need) {
          level = next_boundary;
          vol -= need;
        } else {
          level += vol / (static_cast<double>(pool) * texel_area_m2);
          vol = 0.0;
          break;
        }
      }
      if (level >= wl_cap - 1e-9 || vol <= 0.0) break;
      ++pool;
    }
    for (size_t m = 0; m < pool; ++m) {
      const int i = sorted_members[m];
      const float ground = B.data[i] + S.data[i];
      const float add = std::max(0.0f, static_cast<float>(level) - ground);
      S.data[i] += add;
    }
    poured[static_cast<size_t>(c)] = 1;

    double leftover = vol;
    if (leftover <= 0.0) continue;
    int cell = find_exit(member[0], static_cast<int32_t>(c));
    while (leftover > 0.0) {
      const int32_t cc = comp_of[cell];
      if (cc >= 0) {
        if (!poured[static_cast<size_t>(cc)]) {
          bucket[static_cast<size_t>(cc)] += leftover;
          leftover = 0.0;
          break;
        }
        cell = find_exit(cell, cc);  // already poured: pass through to its exit
        continue;
      }
      const double dep = dry_deposit(cell, leftover, S, area, p, texel_area_m2);
      leftover -= dep;
      const int32_t rcv = r.receiver[cell];
      if (rcv < 0) {
        exported += leftover;
        leftover = 0.0;
        break;
      }
      cell = rcv;
    }
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

Field2D<float> river_intensity(const FlowRouting& r, const Field2D<float>& area,
                               const ErosionParams& p) {
  Field2D<float> out(r.width, r.height, 0.0f);
  const float lo = std::log2(std::max(p.stream_min_area_m2, 1e-6f));
  const float hi = std::log2(std::max(p.river_area_m2, p.stream_min_area_m2 + 1e-6f));
  for (size_t i = 0; i < out.data.size(); ++i) {
    if (r.in_lake[i]) continue;  // the lake IS the water: stays 0
    const float a = area.data[i];
    if (a < p.stream_min_area_m2) continue;  // stays 0
    out.data[i] = glm::smoothstep(lo, hi, std::log2(a));
  }
  return out;
}

namespace {

// Splat each cell's own river intensity onto a square neighborhood whose
// radius grows with that intensity — 0 (a single texel: a faint stream) up
// to kRiverMaxDilationRadius (a river reads as a few-texel-wide band), so the
// artifact looks like a texture rather than a hairline (v1.3 addendum;
// hairline-only would be this constant set to 0). Cells keep at least their
// own raw value; a neighbor's splat only ever raises (max), never lowers.
constexpr int kRiverMaxDilationRadius = 1;  // sim texels

Field2D<float> dilate_river(const Field2D<float>& intensity) {
  const int w = intensity.width, h = intensity.height;
  Field2D<float> out = intensity;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float t = intensity.at(x, y);
      if (t <= 0.0f) continue;
      const int radius =
          static_cast<int>(std::lround(t * static_cast<float>(kRiverMaxDilationRadius)));
      if (radius <= 0) continue;
      const int x0 = std::max(0, x - radius), x1 = std::min(w - 1, x + radius);
      const int y0 = std::max(0, y - radius), y1 = std::min(h - 1, y + radius);
      for (int ny = y0; ny <= y1; ++ny)
        for (int nx = x0; nx <= x1; ++nx) {
          float& o = out.at(nx, ny);
          o = std::max(o, t);
        }
    }
  }
  return out;
}

}  // namespace

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
  out.river = dilate_river(river_intensity(r, out.flow, p));
  // Dilation splats from non-lake neighbors and can reach into an in_lake
  // cell; river_intensity() already zeroed lake cells pre-dilation, so
  // re-zero here to keep the invariant after the splat too.
  for (size_t i = 0; i < out.river.data.size(); ++i)
    if (r.in_lake[i]) out.river.data[i] = 0.0f;
  return out;
}

}  // namespace badlands::mapgen
