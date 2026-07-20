// Pure-CPU tests for the terrain mesh builder over MapData (no GPU).

#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <glm/glm.hpp>

#include "game/geometry/terrain_mesh.hpp"
#include "game/map/map_data.hpp"

using namespace badlands;
using badlands::mapgen::Biome;

namespace {

constexpr float kSpacing = 4.0f;

struct Vertex {
  glm::vec3 pos;
  glm::vec3 normal;
  std::array<uint8_t, 4> indices;  // Uint8x4
  std::array<uint8_t, 4> weights;  // Unorm8x4 (0..255)
};

Vertex Unpack(const std::vector<float>& d, size_t i) {
  const float* p = d.data() + i * TerrainMesh::kFloatsPerVertex;
  Vertex v;
  v.pos = {p[0], p[1], p[2]};
  v.normal = {p[3], p[4], p[5]};
  uint32_t idx, w;
  std::memcpy(&idx, &p[6], 4);
  std::memcpy(&w, &p[7], 4);
  for (int k = 0; k < 4; ++k) {
    v.indices[k] = static_cast<uint8_t>((idx >> (8 * k)) & 0xffu);
    v.weights[k] = static_cast<uint8_t>((w >> (8 * k)) & 0xffu);
  }
  return v;
}

int NonZero(const Vertex& v) {
  int n = 0;
  for (int k = 0; k < 4; ++k)
    if (v.weights[k] > 0) ++n;
  return n;
}

int SumWeights(const Vertex& v) {
  int s = 0;
  for (int k = 0; k < 4; ++k) s += v.weights[k];
  return s;
}

}  // namespace

TEST_CASE("BuildTerrainMesh: X-split topology and normalized weights") {
  // Flat ground, hard left/right biome split (one-hot slices on every node).
  constexpr int kNodes = 5;
  MapData map(kNodes, kNodes, kSpacing);
  for (int j = 0; j < kNodes; ++j) {
    for (int i = 0; i < kNodes; ++i) {
      map.mutable_height(i, j) = 5.0f;
      const int b = (i < kNodes / 2) ? static_cast<int>(Biome::Lake)
                                     : static_cast<int>(Biome::Swamp);
      map.mutable_slice(b, i, j) = 255;
    }
  }

  const TerrainMesh mesh = BuildTerrainMesh(map);

  // Indexed + X-split: one shared vertex per lattice node plus one per cell
  // centre; 4 triangles per cell. Mesh density IS lattice density.
  const int cells_per_side = kNodes - 1;
  const int corner_count = kNodes * kNodes;
  const int cells = cells_per_side * cells_per_side;
  REQUIRE(mesh.vertex_count == static_cast<uint32_t>(corner_count + cells));
  REQUIRE(mesh.vertices.size() ==
          mesh.vertex_count * TerrainMesh::kFloatsPerVertex);
  REQUIRE(mesh.indices.size() == static_cast<size_t>(cells) * 4 * 3);

  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);
    // Flat terrain -> every node at height 5, up-facing normal.
    CHECK(v.pos.y == Catch::Approx(5.0f));
    CHECK(v.normal.y == Catch::Approx(1.0f).margin(1e-4));
    // Every vertex is a fully normalized blend, whatever biomes it mixes.
    CHECK(SumWeights(v) == 255);
    for (int k = 0; k < 4; ++k) {
      if (v.weights[k] > 0) {
        CHECK((v.indices[k] == static_cast<uint8_t>(Biome::Lake) ||
               v.indices[k] == static_cast<uint8_t>(Biome::Swamp)));
      }
    }
  }

  // Corner vertices sit on one-hot nodes, so they stay one-hot...
  for (uint32_t i = 0; i < static_cast<uint32_t>(corner_count); ++i) {
    INFO("corner vertex " << i);
    CHECK(NonZero(Unpack(mesh.vertices, i)) == 1);
  }
  // ...while a centre vertex straddling the border averages its 4 corners and
  // comes out genuinely BLENDED. The old builder could only ever emit one-hot,
  // which is exactly what made straight borders staircase.
  bool saw_blended_centre = false;
  for (uint32_t i = corner_count; i < mesh.vertex_count; ++i) {
    if (NonZero(Unpack(mesh.vertices, i)) == 2) saw_blended_centre = true;
  }
  CHECK(saw_blended_centre);

  // A boundary triangle references vertices carrying both biomes.
  bool saw_boundary_tri = false;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    bool has_lake = false, has_swamp = false;
    for (int k = 0; k < 3; ++k) {
      const Vertex v = Unpack(mesh.vertices, mesh.indices[t + k]);
      for (int s = 0; s < 4; ++s) {
        if (v.weights[s] == 0) continue;
        has_lake |= v.indices[s] == static_cast<uint8_t>(Biome::Lake);
        has_swamp |= v.indices[s] == static_cast<uint8_t>(Biome::Swamp);
      }
    }
    if (has_lake && has_swamp) saw_boundary_tri = true;
  }
  CHECK(saw_boundary_tri);
}

TEST_CASE("BuildTerrainMesh: vertex heights follow the heightmap") {
  // A west->east height ramp: h(x, z) = x meters, on a 1 m lattice so world x
  // equals the node index.
  constexpr int kNodesX = 9;
  constexpr int kNodesZ = 5;
  MapData map(kNodesX, kNodesZ, 1.0f);
  for (int j = 0; j < kNodesZ; ++j)
    for (int i = 0; i < kNodesX; ++i)
      map.mutable_height(i, j) = static_cast<float>(i);

  const TerrainMesh mesh = BuildTerrainMesh(map);
  REQUIRE(mesh.vertex_count > 0);

  float min_y = 1e9f, max_y = -1e9f;
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);
    min_y = std::min(min_y, v.pos.y);
    max_y = std::max(max_y, v.pos.y);
    if (v.pos.x > 2.0f && v.pos.x < static_cast<float>(kNodesX - 3)) {
      CHECK(v.pos.y == Catch::Approx(v.pos.x).margin(0.01));
      // h = x ramp -> slope +1 in x, 0 in z -> normal ~ normalize(-1, 1, 0).
      CHECK(v.normal.x < -0.5f);
      CHECK(v.normal.y > 0.5f);
      CHECK(std::abs(v.normal.z) < 0.05f);
    }
  }
  // The ramp spans its full range: nodes run x = 0 .. kNodesX-1.
  CHECK(max_y - min_y ==
        Catch::Approx(static_cast<float>(kNodesX - 1)).margin(0.5));
}

TEST_CASE("BuildTerrainMesh: cell-region chunking covers the map exactly") {
  constexpr int kNodes = 9;
  MapData map(kNodes, kNodes, kSpacing);
  for (int j = 0; j < kNodes; ++j)
    for (int i = 0; i < kNodes; ++i)
      map.mutable_slice(static_cast<int>(Biome::Plains), i, j) = 255;

  const TerrainMesh whole = BuildTerrainMesh(map);
  size_t chunked_tris = 0;
  constexpr int kChunk = 3;  // deliberately not a divisor of 8 cells
  for (int cz = 0; cz < kNodes - 1; cz += kChunk) {
    for (int cx = 0; cx < kNodes - 1; cx += kChunk) {
      TerrainMeshParams p;
      p.cell_x0 = cx;
      p.cell_z0 = cz;
      p.cells_x = std::min(kChunk, (kNodes - 1) - cx);
      p.cells_z = std::min(kChunk, (kNodes - 1) - cz);
      chunked_tris += BuildTerrainMesh(map, p).indices.size() / 3;
    }
  }
  CHECK(chunked_tris == whole.indices.size() / 3);
}

TEST_CASE("BuildTerrainMesh: an empty map is not a crash") {
  const MapData empty;
  const TerrainMesh mesh = BuildTerrainMesh(empty);
  CHECK(mesh.vertex_count == 0);
  CHECK(mesh.indices.empty());
}
