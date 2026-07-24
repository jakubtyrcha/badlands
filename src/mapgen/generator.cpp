#include "mapgen/generator.hpp"

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

#include <FastNoiseLite.h>

#include "mapgen/biomes.hpp"
#include "mapgen/parallel.hpp"

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

MapArtifacts generate_map(const MapGenParams& params) {
  const int w = params.resolution.x, h = params.resolution.y;
  if (w <= 0 || h <= 0) {
    MapArtifacts a;
    return a;
  }
  MapArtifacts a;
  a.bedrock = Field2D<float>(w, h);
  a.heightmap = Field2D<float>(w, h, 0.0f);

  // Sample at world = x * texel_m (node convention): coinciding world points
  // across two resolutions of the same map get identical float inputs.
  const glm::vec2 texel(params.size_m.x / static_cast<float>(w),
                        params.size_m.y / static_cast<float>(h));

  // Distinct derived seeds per layer, all from params.seed.
  // Use unsigned arithmetic to avoid signed overflow for edge-case seeds.
  const FastNoiseLite base =
      make_noise(static_cast<int>(params.seed), kBaseWavelengthM, kBaseOctaves,
                 FastNoiseLite::FractalType_FBm);
  const FastNoiseLite ridged =
      make_noise(static_cast<int>(params.seed + 1u), kRidgedWavelengthM,
                 kRidgedOctaves, FastNoiseLite::FractalType_Ridged);
  const FastNoiseLite belt =
      make_noise(static_cast<int>(params.seed + 2u),
                 std::max(params.size_m.x, params.size_m.y), 1,
                 FastNoiseLite::FractalType_FBm);

  // GetNoise is const and stateless per call, so the three sources are shared
  // read-only across the workers; tiles write disjoint pixels.
  parallel_tiles(
      w, h, 64, [] { return std::monostate{}; },
      [&](std::monostate&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const float wx = static_cast<float>(x) * texel.x;
            const float wy = static_cast<float>(y) * texel.y;
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
            a.bedrock.at(x, y) = b;
          }
        }
      });

  a.biome = classify_biomes(a.bedrock, compute_cutoffs(a.bedrock));

  // First-pass relief: a cone field over the distance to the nearest plains.
  // Ridge crests emerge along the mountain belts' medial axes; detail,
  // erosion and water come later (see the heightmap spec).
  const Field2D<float> dist = distance_to_plains(a.biome, texel);
  for (size_t i = 0; i < dist.data.size(); ++i)
    a.heightmap.data[i] = kSlopeMPerM * dist.data[i];

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

Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m) {
  const int w = biome.width, h = biome.height;
  if (w <= 0 || h <= 0) return Field2D<float>{};
  Field2D<float> out(w, h, 0.0f);

  const auto kPlains = static_cast<uint8_t>(Biome::Plains);
  bool any_plains = false;
  for (uint8_t b : biome.data) {
    if (b == kPlains) {
      any_plains = true;
      break;
    }
  }
  if (!any_plains) return out;  // documented degenerate: all zeros

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
                       s.f[y] = biome.at(x, y) == kPlains ? 0.0 : kBigD;
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

}  // namespace badlands::mapgen
