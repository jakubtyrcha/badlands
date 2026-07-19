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

// Pack four u8 into one float slot (matches the Uint8x4 / Unorm8x4 attributes).
float PackU8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint32_t u = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
                     (static_cast<uint32_t>(c) << 16) |
                     (static_cast<uint32_t>(d) << 24);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

// Emit one node as a vertex. Each grid node is a single biome (one-hot: pair 0
// = {biome, weight 255}); the blend across a triangle comes from interpolating
// these one-hots between differently-biomed vertices in the vertex shader.
void EmitVertex(std::vector<float>& out, const Field2D<float>& h,
                const Node& node) {
  const glm::vec3 n = NormalAt(h, node.pos.x, node.pos.z);
  out.insert(out.end(), {node.pos.x, node.pos.y, node.pos.z, n.x, n.y, n.z});
  out.push_back(PackU8x4(node.biome, 0, 0, 0));  // layer_indices
  out.push_back(PackU8x4(255, 0, 0, 0));         // blend_weights (one-hot)
}

}  // namespace

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

bool RaycastTerrain(const Field2D<float>& heightmap, const Ray& ray,
                    glm::vec3& out_hit) {
  if (heightmap.width <= 0 || heightmap.height <= 0) return false;

  const float mps = static_cast<float>(kMetersPerSample);
  const float max_x = (heightmap.width - 1) * mps;
  const float max_z = (heightmap.height - 1) * mps;
  // Bound the march by the map diagonal + vertical span: past that the ray can
  // only be leaving.
  const float span = std::sqrt(max_x * max_x + max_z * max_z);
  const float kStep = mps;  // one sample per step -- can't skip a whole cell
  const float kMaxDist = span * 2.0f;

  auto inside_xz = [&](const glm::vec3& p) {
    return p.x >= 0.0f && p.z >= 0.0f && p.x <= max_x && p.z <= max_z;
  };
  // Signed height above the surface; negative once the ray is underground.
  auto above = [&](const glm::vec3& p) {
    return p.y - SampleHeight(heightmap, p.x, p.z);
  };

  glm::vec3 prev = ray.origin;
  // Starting underground (camera inside a hill): nothing sensible to pick.
  if (inside_xz(prev) && above(prev) < 0.0f) return false;

  for (float t = kStep; t <= kMaxDist; t += kStep) {
    const glm::vec3 cur = ray.At(t);
    const float cur_above = above(cur);

    if (inside_xz(cur) && cur_above <= 0.0f) {
      // Crossed the surface between prev and cur -- bisect for the crossing.
      glm::vec3 lo = prev, hi = cur;
      for (int i = 0; i < 24; ++i) {
        const glm::vec3 mid = (lo + hi) * 0.5f;
        if (above(mid) > 0.0f) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      out_hit = (lo + hi) * 0.5f;
      // Snap y onto the surface: bisection converges on t, and the ray may be
      // shallow enough that a small t error shows up in y.
      out_hit.y = SampleHeight(heightmap, out_hit.x, out_hit.z);
      return true;
    }
    prev = cur;
  }
  return false;
}

TerrainMesh BuildTerrainMesh(const Field2D<float>& heightmap,
                             const Field2D<uint8_t>& biome,
                             const TerrainMeshParams& params) {
  const int subdiv = std::max(1, params.subdiv);
  const int blocks_total_x = heightmap.width / kSamplesPerBlock;
  const int blocks_total_z = heightmap.height / kSamplesPerBlock;
  const int bx0 = std::clamp(params.block_x0, 0, blocks_total_x);
  const int bz0 = std::clamp(params.block_z0, 0, blocks_total_z);
  const int bw = params.blocks_x < 0 ? blocks_total_x - bx0
                                     : std::min(params.blocks_x,
                                                blocks_total_x - bx0);
  const int bh = params.blocks_z < 0 ? blocks_total_z - bz0
                                     : std::min(params.blocks_z,
                                                blocks_total_z - bz0);
  const int cells_w = bw * subdiv;      // cells across the region
  const int cells_h = bh * subdiv;
  const int cx0 = bx0 * subdiv;         // absolute cell offset of the region
  const int cz0 = bz0 * subdiv;
  const float step = static_cast<float>(kBlockSizeM) / subdiv;  // world meters
  const int corner_cols = cells_w + 1;
  const int corner_rows = cells_h + 1;
  const int corner_count = corner_cols * corner_rows;

  // cx/cz are ABSOLUTE cell coords -> world meters (chunks share a world frame).
  auto make_node = [&](float cx, float cz) {
    const float wx = cx * step;
    const float wz = cz * step;
    return Node{glm::vec3(wx, SampleHeight(heightmap, wx, wz), wz),
                SampleBiome(biome, wx, wz)};
  };

  TerrainMesh mesh;
  // Shared vertices: one per corner node + one per cell centre (X-split).
  mesh.vertices.reserve(
      static_cast<size_t>(corner_count + cells_w * cells_h) *
      TerrainMesh::kFloatsPerVertex);
  for (int lz = 0; lz < corner_rows; ++lz)
    for (int lx = 0; lx < corner_cols; ++lx)
      EmitVertex(mesh.vertices, heightmap,
                 make_node(cx0 + lx, cz0 + lz));
  for (int lz = 0; lz < cells_h; ++lz)
    for (int lx = 0; lx < cells_w; ++lx)
      EmitVertex(mesh.vertices, heightmap,
                 make_node(cx0 + lx + 0.5f, cz0 + lz + 0.5f));

  auto corner_idx = [&](int lx, int lz) {
    return static_cast<uint32_t>(lz * corner_cols + lx);
  };
  auto center_idx = [&](int lx, int lz) {
    return static_cast<uint32_t>(corner_count + lz * cells_w + lx);
  };

  mesh.indices.reserve(static_cast<size_t>(cells_w) * cells_h * 4 * 3);
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  };
  for (int lz = 0; lz < cells_h; ++lz) {
    for (int lx = 0; lx < cells_w; ++lx) {
      const uint32_t n00 = corner_idx(lx, lz);
      const uint32_t n10 = corner_idx(lx + 1, lz);
      const uint32_t n11 = corner_idx(lx + 1, lz + 1);
      const uint32_t n01 = corner_idx(lx, lz + 1);
      const uint32_t nc = center_idx(lx, lz);
      // X-split: 4 triangles around the cell centre (CCW seen from +Y).
      tri(n00, nc, n10);
      tri(n10, nc, n11);
      tri(n11, nc, n01);
      tri(n01, nc, n00);
    }
  }

  mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() /
                                            TerrainMesh::kFloatsPerVertex);
  return mesh;
}

}  // namespace badlands
