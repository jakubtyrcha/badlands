// Pure-CPU tests for the tessellated-block terrain mesh builder (no GPU).

#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <glm/glm.hpp>

#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/mapgen_constants.hpp"

using namespace badlands;
using badlands::mapgen::Field2D;
using badlands::mapgen::kSamplesPerBlock;

namespace {

struct Vertex {
  glm::vec3 pos;
  glm::vec3 normal;
  glm::vec4 weights;
  std::array<uint32_t, 4> indices;
};

Vertex Unpack(const std::vector<float>& d, size_t i) {
  const float* p = d.data() + i * TerrainMesh::kFloatsPerVertex;
  Vertex v;
  v.pos = {p[0], p[1], p[2]};
  v.normal = {p[3], p[4], p[5]};
  v.weights = {p[6], p[7], p[8], p[9]};
  for (int k = 0; k < 4; ++k) std::memcpy(&v.indices[k], &p[10 + k], 4);
  return v;
}

}  // namespace

TEST_CASE("BuildTerrainMesh: X-split counts, one-hot weights, biome boundary") {
  // 2x2 blocks of flat terrain; left half biome 0, right half biome 1.
  const int W = 2 * kSamplesPerBlock;  // 20
  const int H = 2 * kSamplesPerBlock;
  Field2D<float> height(W, H, 5.0f);
  Field2D<uint8_t> biome(W, H);
  for (int z = 0; z < H; ++z)
    for (int x = 0; x < W; ++x)
      biome.at(x, z) = static_cast<uint8_t>(x < W / 2 ? 0 : 1);

  TerrainMesh mesh = BuildTerrainMesh(height, biome, {.subdiv = 2});

  // cells = (2*2) x (2*2) = 16; X-split = 4 tris/cell x 3 verts = 12/cell.
  REQUIRE(mesh.vertex_count == 16u * 12u);
  REQUIRE(mesh.vertices.size() ==
          mesh.vertex_count * TerrainMesh::kFloatsPerVertex);

  bool saw_boundary_tri = false;
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);

    // Flat terrain -> every node at height 5, up-facing normal.
    CHECK(v.pos.y == Catch::Approx(5.0f));
    CHECK(v.normal.y == Catch::Approx(1.0f).margin(1e-4));

    // Weights are one-hot and sum to 1 (each node belongs to a single biome).
    const float wsum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
    CHECK(wsum == Catch::Approx(1.0f));
    int hot = 0;
    for (int k = 0; k < 4; ++k)
      if (v.weights[k] > 0.5f) ++hot;
    CHECK(hot == 1);
  }

  // Every triangle's three vertices share one index set; a boundary triangle
  // spans both biome 0 and 1.
  for (uint32_t t = 0; t < mesh.vertex_count; t += 3) {
    const auto& idx = Unpack(mesh.vertices, t).indices;
    const bool has0 = idx[0] == 0 || idx[1] == 0 || idx[2] == 0 || idx[3] == 0;
    const bool has1 = idx[0] == 1 || idx[1] == 1 || idx[2] == 1 || idx[3] == 1;
    if (has0 && has1) saw_boundary_tri = true;
  }
  CHECK(saw_boundary_tri);
}

TEST_CASE("BuildTerrainMesh: vertex heights follow the heightmap") {
  // A west->east height ramp: h(x,z) = x meters.
  const int W = 2 * kSamplesPerBlock;
  const int H = 1 * kSamplesPerBlock;
  Field2D<float> height(W, H);
  for (int z = 0; z < H; ++z)
    for (int x = 0; x < W; ++x) height.at(x, z) = static_cast<float>(x);
  Field2D<uint8_t> biome(W, H);  // all biome 0

  TerrainMesh mesh = BuildTerrainMesh(height, biome, {.subdiv = 4});
  REQUIRE(mesh.vertex_count > 0);

  // Interior nodes sample the ramp: y ~= world x (edge nodes clamp, skip them).
  float min_y = 1e9f, max_y = -1e9f;
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);
    min_y = std::min(min_y, v.pos.y);
    max_y = std::max(max_y, v.pos.y);
    if (v.pos.x > 1.0f && v.pos.x < static_cast<float>(W - 2)) {
      CHECK(v.pos.y == Catch::Approx(v.pos.x).margin(0.01));
    }
  }
  CHECK(max_y - min_y > 12.0f);  // the ramp really spans a range
}
