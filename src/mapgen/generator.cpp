#include "mapgen/generator.hpp"

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

#include <FastNoiseLite.h>

#include "mapgen/biomes.hpp"
#include "mapgen/canal_carve.hpp"
#include "mapgen/detail_filter.hpp"
#include "mapgen/erosion.hpp"
#include "mapgen/hydrology.hpp"
#include "mapgen/parallel.hpp"
#include "mapgen/resample.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

namespace {

// Fixed generation constants (tuned by eye via --preview-image-only).
// Wavelengths are world METERS — generation is resolution-independent.
constexpr float kBaseWavelengthM = 250.0f;    // rolling continental base
constexpr int kBaseOctaves = 4;
constexpr float kRidgedWavelengthM = 230.0f;  // elongated crest lines
constexpr int kRidgedOctaves = 4;
constexpr float kRidgeWeight = 2.5f;  // how far crests rise above the base
// Sharpens the ridged term before it's weighted in, concentrating the top
// quantile onto crest lines instead of the whole ridged-field envelope.
constexpr float kRidgeSharpness = 3.5f;
// The belt mask gates where ridges may appear (a few mountain belts per map,
// not everywhere). Its wavelength is the map's own extent, so this is a
// single sample of ~1 noise period per map: the field's realized range
// varies a lot by seed (observed raw max from 0.63 to 0.82 across 10 seeds
// at these params) rather than reliably spanning [-1,1]. kBeltLo/kBeltHi are
// an absolute threshold, not a quantile, so coverage is seed-dependent by
// construction; these values were picked to keep coverage non-zero but
// non-dominant across seeds 1-3.
constexpr float kBeltLo = 0.47f;
constexpr float kBeltHi = 0.59f;
// High-level relief slope: height meters per meter of horizontal WORLD
// distance to the nearest plains (NOT per texel — regenerating at another
// resolution must not change slopes; the units-guard test pins this).
// Plains sit at the 0 m water datum; the farthest texel is the highest.
constexpr float kSlopeMPerM = 0.75f;

// v1.1: plains drainage relief. Cavities carve at bedrock minima, so the
// bedrock field is a ready-made potential whose valleys already lead to the
// lakes — this term nudges the plains gradient to point at them instead of
// sitting dead flat, without a seam at any biome cutoff (see the v1.1
// addendum, "Plains drainage relief", in
// docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md).
constexpr float kPlainsReliefM = 2.0f;

FastNoiseLite make_noise(int seed, float wavelength_m, int octaves,
                         FastNoiseLite::FractalType fractal) {
  FastNoiseLite n(seed);
  n.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  n.SetFractalType(fractal);
  n.SetFractalOctaves(octaves);
  n.SetFrequency(1.0f / wavelength_m);
  return n;
}

float to01(float v) { return 0.5f * (v + 1.0f); }  // FastNoiseLite is ~[-1,1]

// Exact k-th order statistic (nth_element-on-copy) — the same pattern as
// compute_cutoffs, mirroring carve_cavities' internal t_lake formula exactly
// (same frac clamp, same rank index) so this recovers the identical cutoff
// carve_cavities uses, without refactoring carve_cavities to expose it.
float bedrock_quantile(const Field2D<float>& bedrock, float frac) {
  const size_t n = bedrock.data.size();
  if (n == 0) return 0.0f;
  std::vector<float> v = bedrock.data;
  const float f = std::min(frac, 1.0f);
  const size_t i = static_cast<size_t>(f * static_cast<float>(n - 1));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  return v[i];
}

}  // namespace

BiomeCutoffs compute_cutoffs(const Field2D<float>& bedrock) {
  // Exact k-th order statistics. nth_element's VALUE at a rank is
  // deterministic even though the permutation around it is not.
  std::vector<float> v = bedrock.data;
  const size_t n = v.size();
  BiomeCutoffs c;
  if (n == 0) return c;
  const size_t i_hills = static_cast<size_t>(kPlainsFrac * (n - 1));
  const size_t i_mtn = static_cast<size_t>((1.0f - kMountainFrac) * (n - 1));
  std::nth_element(v.begin(), v.begin() + i_hills, v.end());
  c.t_hills = v[i_hills];
  std::nth_element(v.begin() + i_hills, v.begin() + i_mtn, v.end());
  c.t_mountain = v[i_mtn];
  return c;
}

Field2D<uint8_t> classify_biomes(const Field2D<float>& bedrock,
                                 const BiomeCutoffs& cutoffs) {
  Field2D<uint8_t> biome(bedrock.width, bedrock.height);
  for (size_t i = 0; i < bedrock.data.size(); ++i) {
    const float b = bedrock.data[i];
    biome.data[i] = static_cast<uint8_t>(
        b < cutoffs.t_hills      ? Biome::Plains
        : b < cutoffs.t_mountain ? Biome::Hills
                                 : Biome::Mountain);
  }
  return biome;
}

MapArtifacts generate_map(const MapGenParams& params, MapDebugSink* sink) {
  const int w = params.resolution;
  if (w <= 0 || params.erosion.sim_resolution <= 0) {
    MapArtifacts a;
    return a;
  }
  MapArtifacts a;
  const float texel_out = params.world_size_m / static_cast<float>(w);

  // Distinct derived seeds per layer, all from params.seed. Use unsigned
  // arithmetic to avoid signed overflow for edge-case seeds. GetNoise is
  // const and stateless per call, so these three sources are constructed
  // once and shared read-only across both sampling passes below (and across
  // parallel_tiles workers within each).
  const FastNoiseLite base =
      make_noise(static_cast<int>(params.seed), kBaseWavelengthM, kBaseOctaves,
                 FastNoiseLite::FractalType_FBm);
  const FastNoiseLite ridged =
      make_noise(static_cast<int>(params.seed + 1u), kRidgedWavelengthM,
                 kRidgedOctaves, FastNoiseLite::FractalType_Ridged);
  const FastNoiseLite belt =
      make_noise(static_cast<int>(params.seed + 2u), params.world_size_m, 1,
                 FastNoiseLite::FractalType_FBm);

  // Sample the three-noise bedrock field at world = x * texel + origin (node
  // convention): coinciding world points across two grids get identical
  // float inputs. Used once for the output grid (origin 0) and once for the
  // padded sim grid (origin = -kPadTexels * texel_sim), so the two stay in
  // exact world-space agreement.
  auto sample_bedrock = [&](int n, float texel, float origin) {
    Field2D<float> out(n, n);
    parallel_tiles(
        n, n, 64, [] { return std::monostate{}; },
        [&](std::monostate&, int x0, int y0, int x1, int y1) {
          for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
              const float wx = static_cast<float>(x) * texel + origin;
              const float wy = static_cast<float>(y) * texel + origin;
              const float mask = glm::smoothstep(kBeltLo, kBeltHi,
                                                 to01(belt.GetNoise(wx, wy)));
              float b = to01(base.GetNoise(wx, wy));
              // mask is exactly 0 below kBeltLo (the smoothstep clamps), so
              // skipping the ridged term outside the belt is bit-identical,
              // not an approximation.
              if (mask > 0.0f) {
                b += kRidgeWeight * mask *
                     std::pow(to01(ridged.GetNoise(wx, wy)), kRidgeSharpness);
              }
              out.at(x, y) = b;
            }
          }
        });
    return out;
  };

  // --- output-res bedrock + biome classification (existing behavior) ---
  a.bedrock = sample_bedrock(w, texel_out, 0.0f);
  a.biome = classify_biomes(a.bedrock, compute_cutoffs(a.bedrock));

  // --- sim grid: sim_resolution + 2*kPadTexels, world-aligned with pad ---
  const ErosionParams& ep = params.erosion;
  const int sim_n = ep.sim_resolution + 2 * kPadTexels;
  const float texel_sim = params.world_size_m / static_cast<float>(ep.sim_resolution);
  const float origin_sim = -kPadTexels * texel_sim;  // world x of sim texel 0
  Field2D<float> bedrock_sim = sample_bedrock(sim_n, texel_sim, origin_sim);
  int seq = 0;
  if (sink) sink->dump("bedrock", seq++, bedrock_sim);
  const auto cutoffs_sim = compute_cutoffs(bedrock_sim);
  const auto biome_sim = classify_biomes(bedrock_sim, cutoffs_sim);
  if (sink) sink->dump("biome-sim", seq++, biome_sim);

  // First-pass relief: a cone field over the distance to the nearest plains,
  // built on the SIM biome/grid. Ridge crests emerge along the mountain
  // belts' medial axes; erosion + water carve the rest.
  const Field2D<float> dist = distance_to_plains(biome_sim, {texel_sim, texel_sim});
  Field2D<float> B(sim_n, sim_n);
  for (size_t i = 0; i < B.data.size(); ++i) B.data[i] = kSlopeMPerM * dist.data[i];

  // v1.1: plains drainage relief, applied EVERYWHERE (continuity — no seam at
  // a biome cutoff). t_lake is carve_cavities' own cutoff, recovered here so
  // the relief term's zero and the cavity carve's zero line up exactly:
  // smoothstep is 0 at/below t_lake (where carve_cavities takes over) and
  // saturates to kPlainsReliefM at/above t_hills, with zero slope at both
  // ends, so hills/mountains gain a flat +2 m with no discontinuity.
  const float t_lake = bedrock_quantile(bedrock_sim, ep.lake_frac);
  for (size_t i = 0; i < B.data.size(); ++i)
    B.data[i] += kPlainsReliefM *
                 glm::smoothstep(t_lake, cutoffs_sim.t_hills, bedrock_sim.data[i]);
  if (sink) sink->dump("cone", seq++, B);

  // v1.2: conical basins at 1/3 the mountain cone's slope (user-directed),
  // replacing the old lake_depth_m-capped bowl — see "Conical lake bottoms"
  // in docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md.
  // v1.3: slope doubled again (1/3 -> 2/3, user-directed) — see the v1.3
  // addendum, "Lake tuning".
  const auto basins = carve_cavities(B, bedrock_sim, ep.lake_frac,
                                     kSlopeMPerM * (2.0f / 3.0f), {texel_sim, texel_sim},
                                     ep.notch_depth_m);
  if (sink) sink->dump("cavities", seq++, basins);
  if (sink) sink->dump("cavities-height", seq++, B);

  // Canal pre-carve: cut a drainage skeleton across the plains BEFORE the sim,
  // so the hydrology has real gradients to follow rather than having to invent
  // them. Runs after cavities (basins are attractors and terminals) and before
  // sediment (cuts bedrock, no layer bookkeeping to respect).
  // See docs/superpowers/specs/2026-07-29-mapgen-canal-precarve-design.md.
  const auto canals = carve_canals(B, basins, dist, ep, texel_sim, params.seed);
  if (sink) {
    sink->dump("canals", seq++, B);
  }
  auto S = init_sediment(dist, basins, ep, texel_sim, origin_sim, params.seed);
  if (sink) sink->dump("sediment-init", seq++, S);

  // v1.1: capped micro-fill — kills noise-scale puddles at the source
  // (shallow closed depressions in the init surface) while leaving real
  // basins (deeper than kMicroFillCapM, or seeded cavities) untouched.
  micro_fill(B, S, basins, texel_sim);
  if (sink) sink->dump("micro-fill", seq++, S);

  const auto sim_out = erode(B, S, ep, texel_sim, sink, &basins);

  // --- resample to the output grid (crop = the origin offset) ---
  auto resample = [&](const Field2D<float>& f) {
    return resample_bilinear(f, texel_sim, origin_sim, w, texel_out);
  };
  Field2D<float> ground(sim_n, sim_n);
  for (size_t i = 0; i < ground.data.size(); ++i)
    ground.data[i] = B.data[i] + S.data[i];
  a.heightmap = resample(ground);
  a.sediment = resample(S);
  a.flow = resample(sim_out.flow);

  // Lake identity: nearest-sample, never bilinear — an id is a label, and
  // averaging two lake indices would name a third lake.
  a.lakes = sim_out.lakes;
  a.lake_id = Field2D<int32_t>(w, w, -1);
  for (int y = 0; y < w; ++y)
    for (int x = 0; x < w; ++x) {
      const int sx = std::clamp(
          static_cast<int>(std::lround((x * texel_out - origin_sim) / texel_sim)),
          0, sim_n - 1);
      const int sy = std::clamp(
          static_cast<int>(std::lround((y * texel_out - origin_sim) / texel_sim)),
          0, sim_n - 1);
      a.lake_id.at(x, y) = sim_out.lake_id.at(sx, sy);
    }

  // water: resample the SURFACE (level where wet, ground where dry) and the
  // depth mask; recompute depth against the output ground so shorelines match
  Field2D<float> surface(sim_n, sim_n);
  for (size_t i = 0; i < surface.data.size(); ++i)
    surface.data[i] = ground.data[i] + sim_out.water_depth.data[i];
  const auto surface_out = resample(surface);
  const auto depth_hint = resample(sim_out.water_depth);
  a.water_depth = Field2D<float>(w, w, 0.0f);
  for (size_t i = 0; i < a.water_depth.data.size(); ++i)
    if (depth_hint.data[i] > 0.01f)
      a.water_depth.data[i] =
          std::max(0.0f, surface_out.data[i] - a.heightmap.data[i]);
  if (sink) sink->dump("water", seq++, a.water_depth);

  // --- detail + biome stamp ---
  const auto delta =
      gully_detail_delta(a.heightmap, a.water_depth, texel_out, params.seed, ep);
  if (sink) sink->dump("detail-delta", seq++, delta);
  for (size_t i = 0; i < delta.data.size(); ++i) a.heightmap.data[i] += delta.data[i];

  // --- river network ---
  // Extracted on the SIM grid (that is where the routing lives) but rasterized
  // straight to the output grid from world-space geometry, so resolution
  // independence needs no resampling step.
  Field2D<float> sim_ground(sim_n, sim_n);
  for (size_t i = 0; i < sim_ground.data.size(); ++i)
    sim_ground.data[i] = B.data[i] + S.data[i];
  a.river_graph = extract_river_graph(sim_out.routing, sim_out.flow,
                                      sim_out.water_depth, sim_ground, ep,
                                      texel_sim, origin_sim, &sim_out.lake_id,
                                      &sim_out.lakes);
  auto rasters = rasterize_rivers(a.river_graph, w, texel_out);
  a.river_discharge_m3_s = std::move(rasters.discharge_m3_s);
  a.river_class = std::move(rasters.cls);
  a.river_depth_m = std::move(rasters.depth_m);
  a.river_speed_m_s = std::move(rasters.speed_m_s);
  a.river_flow_dir = std::move(rasters.flow_dir);

  // Lake covers exactly the water. The freeboard in finalize_lakes leaves a
  // band of carved bowl dry, and that band keeps whatever classify_biomes gave
  // it — Plains, at bedrock minima — so a coast exists.
  for (size_t i = 0; i < a.biome.data.size(); ++i)
    if (a.water_depth.data[i] > 0.0f)
      a.biome.data[i] = static_cast<uint8_t>(Biome::Lake);
  if (sink) {
    sink->dump("river", seq++, a.river_class);
    sink->dump("final-height", seq++, a.heightmap);
    sink->dump("biome", seq++, a.biome);
  }
  return a;
}

namespace {

// One 1D pass of the Felzenszwalb–Huttenlocher squared-distance transform:
// given f[i] = best squared WORLD distance already achieved at sample i
// (kBigD = "no seed"), writes d[i] = min_j(f[j] + (step*(i-j))^2) via the
// parabola lower envelope. Double precision so the exact-vs-brute-force
// test guarantee holds at map-scale magnitudes. kBigD is a large FINITE
// value, not infinity: two "empty" parabolas must intersect at a finite
// point or the envelope math produces NaN.
constexpr double kBigD = 1e30;

void dt1d(const std::vector<double>& f, std::vector<double>& d,
          std::vector<int>& v, std::vector<double>& z, int n, double step) {
  const double s2 = step * step;
  int k = 0;
  v[0] = 0;
  z[0] = -kBigD;
  z[1] = kBigD;
  for (int q = 1; q < n; ++q) {
    const double fq = f[q] + s2 * q * q;
    for (;;) {
      const int p = v[k];
      const double s =
          (fq - (f[p] + s2 * p * p)) / (2.0 * s2 * static_cast<double>(q - p));
      if (k > 0 && s <= z[k]) {
        --k;
        continue;
      }
      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = kBigD;
      break;
    }
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < static_cast<double>(q)) ++k;
    const int p = v[k];
    const double dq = step * static_cast<double>(q - p);
    d[q] = f[p] + dq * dq;
  }
}

}  // namespace

Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask,
                                glm::vec2 texel_m) {
  const int w = mask.width, h = mask.height;
  if (w <= 0 || h <= 0) return Field2D<float>{};
  Field2D<float> out(w, h, 0.0f);

  bool any_seed = false;
  for (uint8_t m : mask.data) {
    if (m != 0) {
      any_seed = true;
      break;
    }
  }
  if (!any_seed) return out;  // documented degenerate: all zeros

  // Squared world distances between the two passes.
  std::vector<double> g(static_cast<size_t>(w) * h);

  struct Scratch {
    std::vector<double> f, d, z;
    std::vector<int> v;
  };
  const int n_max = std::max(w, h);
  auto make_scratch = [n_max] {
    Scratch s;
    s.f.resize(static_cast<size_t>(n_max));
    s.d.resize(static_cast<size_t>(n_max));
    s.z.resize(static_cast<size_t>(n_max) + 1);
    s.v.resize(static_cast<size_t>(n_max));
    return s;
  };

  // Pass 1: per COLUMN over y (step = texel_m.y). Columns are independent;
  // parallel_tiles with height 1 hands out x-ranges.
  parallel_tiles(w, 1, 64, make_scratch,
                 [&](Scratch& s, int x0, int, int x1, int) {
                   for (int x = x0; x < x1; ++x) {
                     for (int y = 0; y < h; ++y)
                       s.f[y] = mask.at(x, y) != 0 ? 0.0 : kBigD;
                     dt1d(s.f, s.d, s.v, s.z, h, texel_m.y);
                     for (int y = 0; y < h; ++y)
                       g[static_cast<size_t>(y) * w + x] = s.d[y];
                   }
                 });

  // Pass 2: per ROW over x (step = texel_m.x) on pass 1's result; sqrt out.
  parallel_tiles(h, 1, 64, make_scratch,
                 [&](Scratch& s, int y0, int, int y1, int) {
                   for (int y = y0; y < y1; ++y) {
                     for (int x = 0; x < w; ++x)
                       s.f[x] = g[static_cast<size_t>(y) * w + x];
                     dt1d(s.f, s.d, s.v, s.z, w, texel_m.x);
                     for (int x = 0; x < w; ++x)
                       out.at(x, y) = static_cast<float>(std::sqrt(s.d[x]));
                   }
                 });

  return out;
}

Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m) {
  Field2D<uint8_t> mask(biome.width, biome.height, 0);
  const auto kPlains = static_cast<uint8_t>(Biome::Plains);
  for (size_t i = 0; i < mask.data.size(); ++i)
    mask.data[i] = biome.data[i] == kPlains ? 1 : 0;
  return distance_to_mask(mask, texel_m);
}

}  // namespace badlands::mapgen
