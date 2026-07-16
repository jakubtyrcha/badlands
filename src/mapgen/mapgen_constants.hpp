#pragma once

// Structural constants for the map grid. These are COMPILE-TIME by design:
// the heightmap sample density (1 m) and the block size (10 m) define the grid
// structure and never vary per run. Only the overall map size is runtime
// (MapgenConfig::width/height, set via --resolution). See the milestone-1 plan.

namespace badlands::mapgen {

// Heightmap density: meters between adjacent heightmap samples.
inline constexpr int kMetersPerSample = 1;

// Grid block edge length in meters (the coarse gameplay grid).
inline constexpr int kBlockSizeM = 10;

// Heightmap samples spanning one block edge (10 at 1 m density).
inline constexpr int kSamplesPerBlock = kBlockSizeM / kMetersPerSample;

static_assert(kBlockSizeM % kMetersPerSample == 0,
              "block size must be a whole number of samples");
static_assert(kSamplesPerBlock == 10, "expected 10 samples per block edge");

}  // namespace badlands::mapgen
