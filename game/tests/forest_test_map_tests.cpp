// The synthetic forest test map and the MapData -> TerrainQuery adapter.
//
// These two are the seam between the game's map contract and the foliage
// library, so what is checked here is the CONTRACT: soft slices that sum to 1,
// a coverage field with a real interior and a real outside, terrain gentle
// enough that nothing is rejected for slope, and an adapter that reports
// exactly what the map holds.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>

#include "foliage/depth_field.hpp"  // kCoverageMaskThreshold
#include "foliage/scatter.hpp"      // SlopeDegreesAt
#include "game/map/forest_test_map_generator.hpp"
#include "game/map/map_data_terrain_query.hpp"

using namespace badlands;

TEST_CASE("Forest test map has the advertised lattice", "[foliage][map]") {
  const MapData map = ForestTestMapGenerator(1).Generate();

  CHECK(map.nodes_x() == ForestTestMapGenerator::kNodesPerSide);
  CHECK(map.nodes_z() == ForestTestMapGenerator::kNodesPerSide);
  CHECK(map.spacing_m() == ForestTestMapGenerator::kSpacingM);
  CHECK(map.size_x_m() == Catch::Approx(128.0f));
  CHECK(map.size_z_m() == Catch::Approx(128.0f));
}

TEST_CASE("Biome slices are soft and sum to one", "[foliage][map]") {
  // The whole point of this generator over MakeOneHotMapData: a genuine blend,
  // so the terrain material does not draw a hard biome edge under a soft tree
  // line.
  const MapData map = ForestTestMapGenerator(1).Generate();

  bool saw_intermediate = false;
  for (int j = 0; j < map.nodes_z(); ++j) {
    for (int i = 0; i < map.nodes_x(); ++i) {
      const BiomeWeights w = map.WeightsAtNode(i, j);
      REQUIRE(w.Sum() == Catch::Approx(1.0f).margin(1e-4f));
      const float f = w.w[static_cast<int>(mapgen::Biome::Forest)];
      if (f > 0.05f && f < 0.95f) saw_intermediate = true;
    }
  }
  CHECK(saw_intermediate);  // a one-hot map would have no partial weights
}

TEST_CASE("Coverage has a real interior and a real outside", "[foliage][map]") {
  // If the blobs never saturate there is no forest interior for a canopy to
  // occupy; if they never fall to zero there is no edge for the depth field to
  // measure from. Both failure modes are silent in a screenshot.
  const MapData map = ForestTestMapGenerator(1).Generate();
  const MapDataTerrainQuery query(map, mapgen::Biome::Forest);

  // Measured against the threshold that actually gates placement
  // (foliage::kCoverageMaskThreshold), not against raw coverage: the depth
  // field thresholds the mask there, and depth is what every layer's density
  // curve reads.
  int masked = 0, zero = 0, saturated = 0, total = 0;
  for (float z = 0.0f; z <= 128.0f; z += 1.0f) {
    for (float x = 0.0f; x <= 128.0f; x += 1.0f) {
      const float c = query.CoverageAt(x, z);
      REQUIRE(c >= 0.0f);
      REQUIRE(c <= 1.0f);
      if (c >= foliage::kCoverageMaskThreshold) masked++;
      if (c >= 0.95f) saturated++;
      if (c <= 0.0f) zero++;
      total++;
    }
  }

  const float masked_frac = static_cast<float>(masked) / total;
  const float zero_frac = static_cast<float>(zero) / total;
  const float saturated_frac = static_cast<float>(saturated) / total;
  INFO("masked " << masked_frac << " saturated " << saturated_frac << " zero "
                 << zero_frac);
  // Enough forest to be worth planting, and enough clear ground that the map
  // reads as "a plain with woods on it" rather than "woods".
  CHECK(masked_frac > 0.10f);
  CHECK(masked_frac < 0.50f);
  // A saturated core exists, so there is a real interior for the canopy.
  CHECK(saturated_frac > 0.02f);
  // And coverage genuinely reaches ZERO over much of the map -- gaussian tails
  // would otherwise leave the whole plain faintly forested (see kTailCutoff).
  CHECK(zero_frac > 0.30f);
}

TEST_CASE("Terrain is gentle and well clear of water", "[foliage][map]") {
  const MapData map = ForestTestMapGenerator(1).Generate();
  const MapDataTerrainQuery query(map, mapgen::Biome::Forest);

  float min_h = 1e9f, max_h = -1e9f, max_slope = 0.0f;
  for (float z = 2.0f; z <= 126.0f; z += 1.0f) {
    for (float x = 2.0f; x <= 126.0f; x += 1.0f) {
      const float h = query.HeightAt(x, z);
      min_h = std::min(min_h, h);
      max_h = std::max(max_h, h);
      max_slope = std::max(max_slope, foliage::SlopeDegreesAt(query, x, z));
    }
  }

  INFO("height " << min_h << ".." << max_h << " max slope " << max_slope);
  // Rolling, not flat: a dead-flat map would not exercise ground conforming.
  CHECK(max_h - min_h > 1.0f);
  CHECK(max_h - min_h < 2.0f * ForestTestMapGenerator::kReliefM + 1.0f);
  // Gentle enough that the slope test never rejects, so what a screenshot
  // shows is the forest, not the terrain filtering it.
  CHECK(max_slope < 25.0f);
  // And no water anywhere near the ground.
  CHECK(query.WaterLevelM() < min_h - 1.0f);
}

TEST_CASE("The adapter reports exactly what the map holds", "[foliage][map]") {
  const MapData map = ForestTestMapGenerator(3).Generate();
  const MapDataTerrainQuery query(map, mapgen::Biome::Forest);

  for (float z : {0.0f, 33.5f, 64.0f, 127.9f}) {
    for (float x : {0.0f, 12.25f, 64.0f, 127.9f}) {
      CHECK(query.HeightAt(x, z) == map.HeightAt(x, z));
      CHECK(query.CoverageAt(x, z) ==
            map.BiomesAt(x, z).w[static_cast<int>(mapgen::Biome::Forest)]);
    }
  }
  CHECK(query.WaterLevelM() == map.water_level_m());

  // A different biome is a different query over the same map -- which is what
  // lets a second forest type grow somewhere else with no library change.
  const MapDataTerrainQuery plains(map, mapgen::Biome::Plains);
  CHECK(plains.CoverageAt(64.0f, 46.0f) < query.CoverageAt(64.0f, 46.0f) + 1.0f);
  CHECK(plains.CoverageAt(2.0f, 2.0f) > query.CoverageAt(2.0f, 2.0f));
}

TEST_CASE("Seed varies terrain but not the forest outline", "[foliage][map]") {
  // The blob layout is fixed on purpose: a moving forest outline would make two
  // screenshots incomparable, so the seed must move the ground and nothing else.
  const MapData a = ForestTestMapGenerator(1).Generate();
  const MapData b = ForestTestMapGenerator(2).Generate();

  bool height_differs = false;
  for (int j = 0; j < a.nodes_z(); ++j) {
    for (int i = 0; i < a.nodes_x(); ++i) {
      if (a.height(i, j) != b.height(i, j)) height_differs = true;
      REQUIRE(a.slice(static_cast<int>(mapgen::Biome::Forest), i, j) ==
              b.slice(static_cast<int>(mapgen::Biome::Forest), i, j));
    }
  }
  CHECK(height_differs);
}

TEST_CASE("ForestCoverageAt matches the slices it filled", "[foliage][map]") {
  const MapData map = ForestTestMapGenerator(1).Generate();
  for (int j = 0; j < map.nodes_z(); j += 7) {
    for (int i = 0; i < map.nodes_x(); i += 7) {
      const float wx = static_cast<float>(i) * ForestTestMapGenerator::kSpacingM;
      const float wz = static_cast<float>(j) * ForestTestMapGenerator::kSpacingM;
      const float expected = ForestTestMapGenerator::ForestCoverageAt(wx, wz);
      const float actual =
          map.WeightsAtNode(i, j).w[static_cast<int>(mapgen::Biome::Forest)];
      // Slices are 8-bit, so agreement is to within one quantization step.
      CHECK(actual == Catch::Approx(expected).margin(1.0f / 255.0f + 1e-4f));
    }
  }
}
