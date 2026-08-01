#include "game/map/forest_test_map_generator.hpp"

#include <algorithm>
#include <cmath>

#include <FastNoiseLite.h>

namespace badlands {

float ForestTestMapGenerator::ForestCoverageAt(float x, float z) {
  float sum = 0.0f;
  for (const Blob& b : kBlobs) {
    const float dx = x - b.x;
    const float dz = z - b.z;
    const float d2 = dx * dx + dz * dz;
    sum += b.amplitude * std::exp(-d2 / (2.0f * b.sigma * b.sigma));
  }

  // Truncate the gaussian tails. A gaussian is never actually zero, so without
  // this ~89% of the map carries some forest weight -- measured -- and since
  // the slices are soft, the terrain material would blend forest ground across
  // nearly the whole plain. Subtracting a floor and rescaling gives coverage
  // that genuinely reaches 0 away from the blobs, which is also what the depth
  // field needs in order to have an edge to measure from.
  sum = (sum - kTailCutoff) / (1.0f - kTailCutoff);
  return std::clamp(sum, 0.0f, 1.0f);
}

MapData ForestTestMapGenerator::Generate() const {
  MapData map(kNodesPerSide, kNodesPerSide, kSpacingM);
  map.set_water_level_m(kWaterLevelM);

  FastNoiseLite terrain;
  terrain.SetSeed(static_cast<int>(seed_));
  terrain.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  terrain.SetFractalType(FastNoiseLite::FractalType_FBm);
  terrain.SetFractalOctaves(kOctaves);
  terrain.SetFrequency(1.0f / kWavelengthM);

  const int forest = static_cast<int>(mapgen::Biome::Forest);
  const int plains = static_cast<int>(mapgen::Biome::Plains);

  for (int j = 0; j < kNodesPerSide; ++j) {
    for (int i = 0; i < kNodesPerSide; ++i) {
      const float wx = static_cast<float>(i) * kSpacingM;
      const float wz = static_cast<float>(j) * kSpacingM;

      map.mutable_height(i, j) =
          kBaseHeightM + kReliefM * terrain.GetNoise(wx, wz);

      // Two slices summing to exactly 255, so BiomeWeights::Sum() is 1 and the
      // blend is genuinely soft. Plains takes the remainder rather than being
      // computed independently -- that is what guarantees the sum, in integer
      // arithmetic, with no rounding drift to chase.
      const float coverage = ForestCoverageAt(wx, wz);
      const auto forest_byte =
          static_cast<uint8_t>(std::lround(std::clamp(coverage, 0.0f, 1.0f) * 255.0f));
      map.mutable_slice(forest, i, j) = forest_byte;
      map.mutable_slice(plains, i, j) = static_cast<uint8_t>(255 - forest_byte);
    }
  }

  return map;
}

}  // namespace badlands
