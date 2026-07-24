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

float smoothstep(float lo, float hi, float x) {
  const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
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

MapArtifacts generate_map(const MapGenParams& params) {
  const int w = params.resolution.x, h = params.resolution.y;
  MapArtifacts a;
  a.bedrock = Field2D<float>(w, h);
  a.heightmap = Field2D<float>(w, h, 0.0f);
  a.biome = Field2D<uint8_t>(w, h);
  if (w <= 0 || h <= 0) return a;

  // Sample at world = x * texel_m (node convention): coinciding world points
  // across two resolutions of the same map get identical float inputs.
  const glm::vec2 texel(params.size_m.x / static_cast<float>(w),
                        params.size_m.y / static_cast<float>(h));

  // Distinct derived seeds per layer, all from params.seed.
  const int s = static_cast<int>(params.seed);
  const FastNoiseLite base = make_noise(s, kBaseWavelengthM, kBaseOctaves,
                                        FastNoiseLite::FractalType_FBm);
  const FastNoiseLite ridged =
      make_noise(s + 1, kRidgedWavelengthM, kRidgedOctaves,
                 FastNoiseLite::FractalType_Ridged);
  const FastNoiseLite belt =
      make_noise(s + 2, std::max(params.size_m.x, params.size_m.y), 1,
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
            const float mask = smoothstep(kBeltLo, kBeltHi,
                                          to01(belt.GetNoise(wx, wy)));
            a.bedrock.at(x, y) =
                to01(base.GetNoise(wx, wy)) +
                kRidgeWeight * mask *
                    std::pow(to01(ridged.GetNoise(wx, wy)), kRidgeSharpness);
          }
        }
      });

  a.biome = classify_biomes(a.bedrock, compute_cutoffs(a.bedrock));
  return a;
}

}  // namespace badlands::mapgen
