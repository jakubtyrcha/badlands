#pragma once

// A small authored map whose only job is to have a FOREST on it.
//
// It exists because the procedural generator cannot currently produce one:
// classify_biomes cuts the bedrock quantiles into Plains/Hills/Mountain only
// (see mapgen/biomes.hpp), so Biome::Forest coverage is zero on every generated
// map and a forest plopper has nothing to plant into. Rather than block foliage
// on a biome-generation change, this hands the plopper the two things it
// actually needs -- a heightmap and a forest coverage field -- with everything
// else held deliberately boring.
//
// The terrain is gentle on purpose: a few metres of Perlin roll, no cliffs, no
// water. Slope and water rejection are already pinned by unit tests against
// analytic terrain; what this map is for is judging the FOREST -- clumping, the
// edge band, the species gradient -- without erosion detail competing for
// attention.
//
// Biome slices here are genuinely SOFT (Forest against Plains, summing to 1),
// unlike map_view_view.cpp's one-hot MakeOneHotMapData wrap of generator
// output. That is deliberate: it exercises the blend path, and it keeps the
// terrain material from showing a hard biome edge under a soft tree line.
//
// Pure CPU: MapData + the mapgen Biome enum. No engine, no GPU.

#include <cstdint>

#include "game/map/map_data.hpp"

namespace badlands {

class ForestTestMapGenerator : public MapGenerator {
 public:
  explicit ForestTestMapGenerator(uint32_t seed = 1) : seed_(seed) {}

  MapData Generate() const override;

  // 128 m square at 1 m spacing. One metre because the foliage depth field
  // samples coverage at that resolution anyway, so a coarser lattice would be
  // the thing limiting how finely the forest edge resolves.
  static constexpr int kNodesPerSide = 129;
  static constexpr float kSpacingM = 1.0f;
  static constexpr float kMapSizeM = (kNodesPerSide - 1) * kSpacingM;  // 128 m

  // Perlin fBm terrain: gentle roll about a base height, well clear of the
  // water plane so nothing is rejected for being wet.
  static constexpr float kBaseHeightM = 10.0f;
  static constexpr float kReliefM = 3.0f;      // peak deviation from base
  static constexpr float kWavelengthM = 70.0f;
  static constexpr int kOctaves = 3;

  // No water on this map. Below the lowest possible ground, so the foliage
  // generator's water clearance never fires.
  static constexpr float kWaterLevelM = kBaseHeightM - kReliefM - 5.0f;

  // Forest coverage is the clamped sum of a few fixed 2D gaussians -- "a plain
  // with a few forest blobs". Fixed rather than seeded so the SHAPE is stable
  // while the seed varies the terrain and (via the plopper) the trees: a moving
  // forest outline would make two screenshots incomparable.
  struct Blob {
    float x, z;      // centre, metres
    float sigma;     // metres
    float amplitude;
  };
  // Coverage floor subtracted (then rescaled) so the gaussian tails actually
  // reach zero -- see ForestCoverageAt.
  static constexpr float kTailCutoff = 0.06f;

  static constexpr int kBlobCount = 4;
  static constexpr Blob kBlobs[kBlobCount] = {
      {44.0f, 46.0f, 20.0f, 1.15f},  // the main mass
      {78.0f, 62.0f, 14.0f, 0.95f},  // merges with it into a lobed outline
      {96.0f, 30.0f, 10.0f, 0.80f},  // a detached stand
      {30.0f, 92.0f, 12.0f, 0.85f},  // another, near the far corner
  };

  // Forest coverage at world XZ, in [0, 1]. Exposed so a test can check the
  // map against the same function the generator fills its slices from.
  static float ForestCoverageAt(float x, float z);

 private:
  uint32_t seed_ = 1;
};

}  // namespace badlands
