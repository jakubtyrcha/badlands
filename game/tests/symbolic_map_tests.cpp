// Pure-CPU tests for the symbolic greybox map generator
// (src/game/map/symbolic_map_generator) and the water surface derived from it.
// No GPU: glm + the frozen MapData contract only.

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include "catch_amalgamated.hpp"
#include "game/geometry/water_surface.hpp"
#include "game/map/map_data.hpp"
#include "game/map/symbolic_map_generator.hpp"

using badlands::BiomeWeights;
using badlands::BuildWaterSurfaceTriangles;
using badlands::EaseInOutSine;
using badlands::kBiomeSliceCount;
using badlands::MapData;
using badlands::SymbolicMapGenerator;
using badlands::mapgen::Biome;

namespace {

constexpr float kTile = SymbolicMapGenerator::kTileSizeM;
constexpr float kSpacing = SymbolicMapGenerator::kSpacingM;
constexpr int kCells = SymbolicMapGenerator::kBlocksPerSide;

glm::vec2 TileCenterWorld(int row, int col) {
  return {(col + 0.5f) * kTile, (row + 0.5f) * kTile};
}

// Expected biome per symbolic tile [row][col] (row 0 = north).
constexpr Biome kExpected[5][5] = {
    {Biome::Forest, Biome::Forest, Biome::Swamp, Biome::Swamp, Biome::Swamp},
    {Biome::Forest, Biome::Lake, Biome::Lake, Biome::Lake, Biome::Swamp},
    {Biome::Forest, Biome::Plains, Biome::Lake, Biome::Lake, Biome::Swamp},
    {Biome::Forest, Biome::Plains, Biome::Plains, Biome::Plains, Biome::Swamp},
    {Biome::Forest, Biome::Forest, Biome::Forest, Biome::Forest, Biome::Forest},
};

// The RAW (unsmoothed) tile assignment for a cell centre.
Biome RawCellBiome(int cx, int cz) {
  const float wx = (cx + 0.5f) * kSpacing;
  const float wz = (cz + 0.5f) * kSpacing;
  const int col = std::min(4, static_cast<int>(wx / kTile));
  const int row = std::min(4, static_cast<int>(wz / kTile));
  return kExpected[row][col];
}

bool CellIsLake(const MapData& m, int cx, int cz) {
  if (cx < 0 || cz < 0 || cx >= kCells || cz >= kCells) return false;
  return m.DominantBiomeAt((cx + 0.5f) * kSpacing, (cz + 0.5f) * kSpacing) ==
         Biome::Lake;
}

float NormalY(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
  const glm::vec3 u = b - a, v = c - a;
  return u.z * v.x - u.x * v.z;
}

bool PointInTriangleXZ(const glm::vec2& p, const glm::vec3& a, const glm::vec3& b,
                       const glm::vec3& c) {
  auto edge = [](const glm::vec2& q, const glm::vec3& u, const glm::vec3& v) {
    return (v.x - u.x) * (q.y - u.z) - (v.z - u.z) * (q.x - u.x);
  };
  const float d0 = edge(p, a, b), d1 = edge(p, b, c), d2 = edge(p, c, a);
  const bool neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
  const bool pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
  return !(neg && pos);
}

}  // namespace

TEST_CASE("symbolic map has the expected extent on the block-edge lattice") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  CHECK(m.nodes_x() == SymbolicMapGenerator::kNodesPerSide);
  CHECK(m.nodes_z() == SymbolicMapGenerator::kNodesPerSide);
  CHECK(m.spacing_m() == Catch::Approx(kSpacing));
  CHECK(m.size_x_m() == Catch::Approx(SymbolicMapGenerator::kMapSizeM));
  CHECK(m.water_level_m() ==
        Catch::Approx(SymbolicMapGenerator::kWaterLevel));
}

TEST_CASE("tile interiors keep the biome of the 5x5 symbolic grid") {
  // Border rounding only reaches a few cells in, so tile centres (half a tile
  // from any border) keep their authored biome.
  const MapData m = SymbolicMapGenerator{}.Generate();
  for (int row = 0; row < 5; ++row) {
    for (int col = 0; col < 5; ++col) {
      const glm::vec2 c = TileCenterWorld(row, col);
      INFO("tile row=" << row << " col=" << col);
      CHECK(m.DominantBiomeAt(c.x, c.y) == kExpected[row][col]);
    }
  }
}

TEST_CASE("every node carries a normalized biome blend") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  for (int j = 0; j < m.nodes_z(); ++j) {
    for (int i = 0; i < m.nodes_x(); ++i) {
      const BiomeWeights w = m.WeightsAtNode(i, j);
      INFO("node " << i << "," << j);
      REQUIRE(w.Sum() == Catch::Approx(1.0f).margin(1e-3f));
    }
  }
}

TEST_CASE("biome borders are blended, not hard-edged") {
  // The whole point of the slices: near a border a node mixes several biomes.
  // A purely one-hot map (the old behaviour) would find none of these.
  const MapData m = SymbolicMapGenerator{}.Generate();
  int blended_nodes = 0;
  for (int j = 0; j < m.nodes_z(); ++j) {
    for (int i = 0; i < m.nodes_x(); ++i) {
      const BiomeWeights w = m.WeightsAtNode(i, j);
      int nonzero = 0;
      for (float v : w.w)
        if (v > 0.01f) ++nonzero;
      if (nonzero >= 2) ++blended_nodes;
    }
  }
  CHECK(blended_nodes > 0);
}

TEST_CASE("diffusion smoothing reshapes the regions without adding spikes") {
  // NOTE on measurement: counting axis-aligned cell edges is NOT a roundness
  // measure here -- replacing a 90-degree corner with a 45-degree staircase
  // makes the region rounder while *increasing* that count. So instead assert
  // (a) the smoothing actually reshaped the regions, and (b) the result is
  // locally smooth: no single-cell spikes or pinholes. The positive evidence
  // that borders now run diagonally lives in the water-surface test below
  // (diagonal_cells > 0).
  const MapData m = SymbolicMapGenerator{}.Generate();

  int changed = 0, lake_cells = 0;
  for (int cz = 0; cz < kCells; ++cz)
    for (int cx = 0; cx < kCells; ++cx) {
      const bool lake = CellIsLake(m, cx, cz);
      if (lake) ++lake_cells;
      if (lake != (RawCellBiome(cx, cz) == Biome::Lake)) ++changed;
    }
  CHECK(lake_cells > 0);
  CHECK(changed > 0);  // the borders moved

  // Nothing isolated and nothing fully enclosed: no lone lake cell stranded in
  // land, and no single land cell drowned inside the lake. (A stricter bound
  // would be wrong -- at the 4-tile junctions where Forest/Swamp/Lake meet, a
  // land cell legitimately has three lake neighbours as the shore wraps a
  // corner.)
  for (int cz = 1; cz < kCells - 1; ++cz) {
    for (int cx = 1; cx < kCells - 1; ++cx) {
      const int lake_neighbours = int(CellIsLake(m, cx - 1, cz)) +
                                  int(CellIsLake(m, cx + 1, cz)) +
                                  int(CellIsLake(m, cx, cz - 1)) +
                                  int(CellIsLake(m, cx, cz + 1));
      INFO("cell " << cx << "," << cz);
      if (CellIsLake(m, cx, cz)) {
        REQUIRE(lake_neighbours >= 1);  // not a stranded island
      } else {
        REQUIRE(lake_neighbours <= 3);  // not a drowned pinhole
      }
    }
  }
}

TEST_CASE("terrain is flat except the carved lake basin") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  float lo = 1e9f, hi = -1e9f;
  for (int j = 0; j < m.nodes_z(); ++j) {
    for (int i = 0; i < m.nodes_x(); ++i) {
      const float h = m.height(i, j);
      lo = std::min(lo, h);
      hi = std::max(hi, h);
    }
  }
  // Nothing rises above the plane; the deepest point is the lake floor.
  CHECK(hi == Catch::Approx(SymbolicMapGenerator::kFlatHeight));
  CHECK(lo == Catch::Approx(SymbolicMapGenerator::kLakeBottom).margin(1e-3f));

  // A woodland tile centre is untouched flat ground.
  const glm::vec2 wood = TileCenterWorld(4, 4);
  CHECK(m.HeightAt(wood.x, wood.y) ==
        Catch::Approx(SymbolicMapGenerator::kFlatHeight));
}

TEST_CASE("lake bank crosses the water level inside the lake") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  bool has_dry = false;  // carved but still at/above the water surface
  bool has_wet = false;  // below the water surface
  for (int j = 0; j < m.nodes_z(); ++j) {
    for (int i = 0; i < m.nodes_x(); ++i) {
      const float h = m.height(i, j);
      if (h >= SymbolicMapGenerator::kFlatHeight) continue;  // uncarved
      has_dry |= (h >= SymbolicMapGenerator::kWaterLevel);
      has_wet |= (h < SymbolicMapGenerator::kWaterLevel);
    }
  }
  CHECK(has_dry);
  CHECK(has_wet);
}

TEST_CASE("easeInOutSine hits its endpoints, midpoint, and is monotonic") {
  CHECK(EaseInOutSine(0.0f) == Catch::Approx(0.0f).margin(1e-6f));
  CHECK(EaseInOutSine(1.0f) == Catch::Approx(1.0f).margin(1e-6f));
  CHECK(EaseInOutSine(0.5f) == Catch::Approx(0.5f).margin(1e-6f));
  float prev = -1.0f;
  for (int i = 0; i <= 20; ++i) {
    const float v = EaseInOutSine(i / 20.0f);
    CHECK(v >= prev);
    prev = v;
  }
}

TEST_CASE("water triangles are lattice-aligned, flat, CCW, and in bounds") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  const std::vector<glm::vec3> tris = BuildWaterSurfaceTriangles(m);
  REQUIRE_FALSE(tris.empty());
  REQUIRE(tris.size() % 3 == 0);

  // Every vertex is a cell corner or a cell CENTRE -> a multiple of half a cell
  // on both axes. This is what keeps the water on the terrain's triangulation.
  const float half = kSpacing * 0.5f;
  for (const glm::vec3& v : tris) {
    REQUIRE(v.y == Catch::Approx(m.water_level_m()));
    REQUIRE(v.x >= 0.0f);
    REQUIRE(v.z >= 0.0f);
    REQUIRE(v.x <= m.size_x_m());
    REQUIRE(v.z <= m.size_z_m());
    REQUIRE(std::fmod(v.x, half) == Catch::Approx(0.0f).margin(1e-4f));
    REQUIRE(std::fmod(v.z, half) == Catch::Approx(0.0f).margin(1e-4f));
  }
  for (size_t i = 0; i < tris.size(); i += 3) {
    INFO("triangle " << i / 3);
    REQUIRE(NormalY(tris[i], tris[i + 1], tris[i + 2]) > 0.0f);
  }
}

TEST_CASE("water covers every lake cell, and cuts the shore diagonally") {
  const MapData m = SymbolicMapGenerator{}.Generate();
  const std::vector<glm::vec3> tris = BuildWaterSurfaceTriangles(m);

  int lake_cells = 0;
  for (int cz = 0; cz < kCells; ++cz) {
    for (int cx = 0; cx < kCells; ++cx) {
      if (!CellIsLake(m, cx, cz)) continue;
      ++lake_cells;
      const glm::vec2 c((cx + 0.5f) * kSpacing, (cz + 0.5f) * kSpacing);
      bool covered = false;
      for (size_t i = 0; i < tris.size() && !covered; i += 3) {
        covered = PointInTriangleXZ(c, tris[i], tris[i + 1], tris[i + 2]);
      }
      INFO("lake cell " << cx << "," << cz << " centre uncovered");
      REQUIRE(covered);
    }
  }
  REQUIRE(lake_cells > 0);

  // Land cells with two PERPENDICULAR lake neighbours each contribute a
  // 2-triangle half cell -- the 45-degree shore.
  int diagonal_cells = 0;
  for (int cz = 0; cz < kCells; ++cz) {
    for (int cx = 0; cx < kCells; ++cx) {
      if (CellIsLake(m, cx, cz)) continue;
      const bool n = CellIsLake(m, cx, cz - 1);
      const bool s = CellIsLake(m, cx, cz + 1);
      const bool e = CellIsLake(m, cx + 1, cz);
      const bool w = CellIsLake(m, cx - 1, cz);
      if ((int(n) + int(s) + int(e) + int(w)) == 2 && (n != s)) ++diagonal_cells;
    }
  }
  CHECK(diagonal_cells > 0);
  CHECK(static_cast<int>(tris.size() / 3) ==
        4 * lake_cells + 2 * diagonal_cells);
}
