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

  // Indexed: cells = (2*2)x(2*2) = 16; 5x5 corner nodes + 16 cell centres = 41
  // shared verts; 16 cells x 4 tris x 3 = 192 indices.
  const int corner_count = 5 * 5;
  const int cells = 4 * 4;
  REQUIRE(mesh.vertex_count == static_cast<uint32_t>(corner_count + cells));
  REQUIRE(mesh.vertices.size() ==
          mesh.vertex_count * TerrainMesh::kFloatsPerVertex);
  REQUIRE(mesh.indices.size() == static_cast<size_t>(cells) * 4 * 3);

  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);
    // Flat terrain -> every node at height 5, up-facing normal.
    CHECK(v.pos.y == Catch::Approx(5.0f));
    CHECK(v.normal.y == Catch::Approx(1.0f).margin(1e-4));
    // Each node is one biome: pair 0 = {biome, weight 255}, rest zero.
    CHECK(v.weights[0] == 255);
    CHECK(v.weights[1] == 0);
    CHECK(v.weights[2] == 0);
    CHECK(v.weights[3] == 0);
    CHECK((v.indices[0] == 0 || v.indices[0] == 1));
  }

  // A boundary triangle references vertices of both biome 0 and biome 1.
  bool saw_boundary_tri = false;
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const uint8_t b0 = Unpack(mesh.vertices, mesh.indices[t + 0]).indices[0];
    const uint8_t b1 = Unpack(mesh.vertices, mesh.indices[t + 1]).indices[0];
    const uint8_t b2 = Unpack(mesh.vertices, mesh.indices[t + 2]).indices[0];
    const bool has0 = b0 == 0 || b1 == 0 || b2 == 0;
    const bool has1 = b0 == 1 || b1 == 1 || b2 == 1;
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

  float min_y = 1e9f, max_y = -1e9f;
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const Vertex v = Unpack(mesh.vertices, i);
    min_y = std::min(min_y, v.pos.y);
    max_y = std::max(max_y, v.pos.y);
    if (v.pos.x > 2.0f && v.pos.x < static_cast<float>(W - 3)) {
      CHECK(v.pos.y == Catch::Approx(v.pos.x).margin(0.01));
      // h = x ramp -> slope +1 in x, 0 in z -> normal ~ normalize(-1, 1, 0).
      // (Locks the NormalAt coordinate handling; it computes at the vertex's
      // world position, so this holds for any kMetersPerSample.)
      CHECK(v.normal.x < -0.5f);
      CHECK(v.normal.y > 0.5f);
      CHECK(std::abs(v.normal.z) < 0.05f);
    }
  }
  CHECK(max_y - min_y > 12.0f);  // the ramp really spans a range
}
