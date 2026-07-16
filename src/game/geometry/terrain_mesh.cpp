#include "game/geometry/terrain_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <glm/glm.hpp>

#include "mapgen/biomes.hpp"
#include "mapgen/mapgen_constants.hpp"

namespace badlands {

namespace {

using mapgen::Field2D;
using mapgen::kBlockSizeM;
using mapgen::kMetersPerSample;
using mapgen::kSamplesPerBlock;

// One subgrid node: world position (with sampled height) + its biome.
struct Node {
  glm::vec3 pos;
  uint8_t biome;
};

int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

// All sampling below takes WORLD-meter coordinates and converts to heightmap
// sample space internally (world_m / kMetersPerSample), so the coordinate
// convention is centralized in one place and correct for any sample density.

// Bilinear height (world meters) at world position (wx, wz) meters.
float SampleHeight(const Field2D<float>& h, float wx, float wz) {
  const float mps = static_cast<float>(kMetersPerSample);
  const float sx = std::clamp(wx / mps, 0.0f, static_cast<float>(h.width - 1));
  const float sz = std::clamp(wz / mps, 0.0f, static_cast<float>(h.height - 1));
  const int x0 = static_cast<int>(std::floor(sx));
  const int z0 = static_cast<int>(std::floor(sz));
  const int x1 = std::min(x0 + 1, h.width - 1);
  const int z1 = std::min(z0 + 1, h.height - 1);
  const float tx = sx - x0;
  const float tz = sz - z0;
  const float h00 = h.at(x0, z0), h10 = h.at(x1, z0);
  const float h01 = h.at(x0, z1), h11 = h.at(x1, z1);
  return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
}

// Nearest biome at world position (wx, wz) meters.
uint8_t SampleBiome(const Field2D<uint8_t>& b, float wx, float wz) {
  const float mps = static_cast<float>(kMetersPerSample);
  const int x = clampi(static_cast<int>(std::lround(wx / mps)), 0, b.width - 1);
  const int z = clampi(static_cast<int>(std::lround(wz / mps)), 0, b.height - 1);
  return b.at(x, z);
}

// Surface normal at world position (wx, wz) from central height differences,
// one sample (kMetersPerSample world meters) apart.
glm::vec3 NormalAt(const Field2D<float>& h, float wx, float wz) {
  const float step = static_cast<float>(kMetersPerSample);
  const float hl = SampleHeight(h, wx - step, wz);
  const float hr = SampleHeight(h, wx + step, wz);
  const float hd = SampleHeight(h, wx, wz - step);
  const float hu = SampleHeight(h, wx, wz + step);
  const float d = 2.0f * step;
  return glm::normalize(glm::vec3(-(hr - hl) / d, 1.0f, -(hu - hd) / d));
}

void PushVertex(std::vector<float>& out, const glm::vec3& pos,
                const glm::vec3& nrm, const glm::vec4& w,
                const std::array<uint32_t, 4>& idx) {
  out.insert(out.end(), {pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z, w.x, w.y,
                         w.z, w.w});
  for (uint32_t i : idx) {
    float f;
    std::memcpy(&f, &i, sizeof(float));
    out.push_back(f);
  }
}

// Emit one triangle: its three vertices share the same (<=4) layer-index set
// (the distinct biomes they span), each with a one-hot weight for its biome.
void EmitTriangle(std::vector<float>& out, const Field2D<float>& h,
                  const Node& a, const Node& b, const Node& c) {
  std::array<uint32_t, 4> idx = {0, 0, 0, 0};
  int n = 0;
  auto add = [&](uint8_t bi) {
    for (int i = 0; i < n; ++i)
      if (idx[i] == bi) return;
    if (n < 4) idx[n++] = bi;
  };
  add(a.biome);
  add(b.biome);
  add(c.biome);

  auto emit = [&](const Node& node) {
    glm::vec4 w(0.0f);
    for (int i = 0; i < 4; ++i) {
      if (idx[i] == node.biome) {
        w[i] = 1.0f;
        break;
      }
    }
    PushVertex(out, node.pos, NormalAt(h, node.pos.x, node.pos.z), w, idx);
  };
  emit(a);
  emit(b);
  emit(c);
}

}  // namespace

TerrainMesh BuildTerrainMesh(const Field2D<float>& heightmap,
                             const Field2D<uint8_t>& biome,
                             const TerrainMeshParams& params) {
  const int subdiv = std::max(1, params.subdiv);
  const int blocks_x = heightmap.width / kSamplesPerBlock;
  const int blocks_z = heightmap.height / kSamplesPerBlock;
  const int cells_x = blocks_x * subdiv;
  const int cells_z = blocks_z * subdiv;
  const float step = static_cast<float>(kBlockSizeM) / subdiv;  // world meters

  TerrainMesh mesh;
  mesh.vertices.reserve(static_cast<size_t>(cells_x) * cells_z * 4 * 3 *
                        TerrainMesh::kFloatsPerVertex);

  auto make_node = [&](float cx, float cz) {
    const float wx = cx * step;  // world meters
    const float wz = cz * step;
    return Node{glm::vec3(wx, SampleHeight(heightmap, wx, wz), wz),
                SampleBiome(biome, wx, wz)};
  };

  for (int cz = 0; cz < cells_z; ++cz) {
    for (int cx = 0; cx < cells_x; ++cx) {
      const Node n00 = make_node(cx, cz);
      const Node n10 = make_node(cx + 1, cz);
      const Node n11 = make_node(cx + 1, cz + 1);
      const Node n01 = make_node(cx, cz + 1);
      const Node nc = make_node(cx + 0.5f, cz + 0.5f);
      // X-split: 4 triangles around the cell centre (CCW seen from +Y).
      EmitTriangle(mesh.vertices, heightmap, n00, nc, n10);
      EmitTriangle(mesh.vertices, heightmap, n10, nc, n11);
      EmitTriangle(mesh.vertices, heightmap, n11, nc, n01);
      EmitTriangle(mesh.vertices, heightmap, n01, nc, n00);
    }
  }

  mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() /
                                            TerrainMesh::kFloatsPerVertex);
  return mesh;
}

}  // namespace badlands
