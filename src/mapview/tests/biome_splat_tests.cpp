// Pure-CPU tests for the biome -> splat raster the cluster terrain samples.
// The invariants that matter are the ones a wrong splat would silently break:
// slot index == Biome enum value (or every biome wears the wrong texture),
// weights sum to full (or the blend darkens), and at most two layers per texel
// (or the fragment shader's fetch count is unbounded).

#include <catch_amalgamated.hpp>

#include "mapgen/biomes.hpp"
#include "mapview/biome_splat.hpp"

using namespace badlands;

namespace {

mapgen::Field2D<uint8_t> UniformBiome(int w, int h, mapgen::Biome b) {
  return mapgen::Field2D<uint8_t>(w, h, static_cast<uint8_t>(b));
}

// Slot weight (0..255) for `slot` at texel (x, y).
int SlotAt(const BiomeSplat& s, int x, int y, int slot) {
  const size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
  return slot < 4 ? s.slots0[i + slot] : s.slots1[i + (slot - 4)];
}

int SlotSum(const BiomeSplat& s, int x, int y) {
  int sum = 0;
  for (int k = 0; k < 8; ++k) sum += SlotAt(s, x, y, k);
  return sum;
}

int NonZeroSlots(const BiomeSplat& s, int x, int y) {
  int n = 0;
  for (int k = 0; k < 8; ++k) {
    if (SlotAt(s, x, y, k) > 0) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("an empty biome field yields an empty splat", "[splat]") {
  const BiomeSplat s = BuildBiomeSplat(mapgen::Field2D<uint8_t>{}, 1.0f);
  CHECK(s.empty());
}

TEST_CASE("slot index is the Biome enum value", "[splat]") {
  // Lake is enum 0 -> slot 0; Mountain is enum 5 -> slot 5 (i.e. slots1.g).
  const BiomeSplat lake =
      BuildBiomeSplat(UniformBiome(16, 16, mapgen::Biome::Lake), 1.0f);
  CHECK(SlotAt(lake, 8, 8, 0) == 255);
  CHECK(SlotAt(lake, 8, 8, 5) == 0);

  const BiomeSplat mountain =
      BuildBiomeSplat(UniformBiome(16, 16, mapgen::Biome::Mountain), 1.0f);
  CHECK(SlotAt(mountain, 8, 8, 5) == 255);
  CHECK(SlotAt(mountain, 8, 8, 0) == 0);
}

TEST_CASE("weights sum to full at every texel", "[splat]") {
  mapgen::Field2D<uint8_t> b(32, 32, static_cast<uint8_t>(mapgen::Biome::Plains));
  for (int y = 0; y < 32; ++y) {
    for (int x = 16; x < 32; ++x) {
      b.at(x, y) = static_cast<uint8_t>(mapgen::Biome::Forest);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      INFO("texel " << x << "," << y);
      CHECK(SlotSum(s, x, y) == 255);
    }
  }
}

TEST_CASE("at most two layers are non-zero per texel", "[splat]") {
  // Four biomes meeting at one corner is the worst case for the top-2 cull.
  mapgen::Field2D<uint8_t> b(32, 32, 0);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      const bool right = x >= 16;
      const bool bottom = y >= 16;
      mapgen::Biome v = mapgen::Biome::Plains;
      if (right && !bottom) v = mapgen::Biome::Forest;
      if (!right && bottom) v = mapgen::Biome::Hills;
      if (right && bottom) v = mapgen::Biome::Mountain;
      b.at(x, y) = static_cast<uint8_t>(v);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      INFO("texel " << x << "," << y);
      CHECK(NonZeroSlots(s, x, y) <= 2);
    }
  }
}

TEST_CASE("a boundary blends and the interior stays pure", "[splat]") {
  mapgen::Field2D<uint8_t> b(32, 32, static_cast<uint8_t>(mapgen::Biome::Plains));
  for (int y = 0; y < 32; ++y) {
    for (int x = 16; x < 32; ++x) {
      b.at(x, y) = static_cast<uint8_t>(mapgen::Biome::Forest);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);  // 1 m texels, 3 m blur

  const int plains = static_cast<int>(mapgen::Biome::Plains);
  const int forest = static_cast<int>(mapgen::Biome::Forest);

  // On the seam both are present.
  CHECK(SlotAt(s, 16, 16, plains) > 0);
  CHECK(SlotAt(s, 16, 16, forest) > 0);
  // Well inside each half, only one is.
  CHECK(SlotAt(s, 2, 16, plains) == 255);
  CHECK(SlotAt(s, 29, 16, forest) == 255);
}
