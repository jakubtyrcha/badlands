#include "game/geometry/terrain_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

namespace badlands {

namespace {

// One lattice node resolved into everything a vertex needs. The centre vertex
// is built by averaging four of these, which is why blend lives here as full
// per-biome weights (averaging already-truncated top-4 sets would be lossy).
struct Node {
  glm::vec3 pos{0.0f};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};
  BiomeWeights blend;
};

float PackU8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint32_t u = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
                     (static_cast<uint32_t>(c) << 16) |
                     (static_cast<uint32_t>(d) << 24);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

// Surface normal from central differences of the bilinear heightmap.
glm::vec3 NormalAt(const MapData& map, float wx, float wz) {
  const float d = map.spacing_m();
  const float hl = map.HeightAt(wx - d, wz);
  const float hr = map.HeightAt(wx + d, wz);
  const float hd = map.HeightAt(wx, wz - d);
  const float hu = map.HeightAt(wx, wz + d);
  return glm::normalize(glm::vec3(-(hr - hl) / (2.0f * d), 1.0f,
                                  -(hu - hd) / (2.0f * d)));
}

Node MakeCornerNode(const MapData& map, int i, int j) {
  Node n;
  const float wx = static_cast<float>(i) * map.spacing_m();
  const float wz = static_cast<float>(j) * map.spacing_m();
  n.pos = glm::vec3(wx, map.height(i, j), wz);
  n.normal = NormalAt(map, wx, wz);
  n.blend = map.WeightsAtNode(i, j);
  return n;
}

// The centre vertex is the average of the cell's four corners -- position,
// normal, and biome weights alike.
Node AverageNodes(const Node& a, const Node& b, const Node& c, const Node& d) {
  Node n;
  n.pos = (a.pos + b.pos + c.pos + d.pos) * 0.25f;
  const glm::vec3 sum = a.normal + b.normal + c.normal + d.normal;
  n.normal = glm::length(sum) > 1e-6f ? glm::normalize(sum)
                                      : glm::vec3(0.0f, 1.0f, 0.0f);
  for (int k = 0; k < kBiomeSliceCount; ++k) {
    n.blend.w[k] = (a.blend.w[k] + b.blend.w[k] + c.blend.w[k] + d.blend.w[k]) *
                   0.25f;
  }
  return n;
}

void EmitVertex(std::vector<float>& out, const Node& node) {
  const VertexBlend vb = ResolveVertexBlend(node.blend);
  out.insert(out.end(), {node.pos.x, node.pos.y, node.pos.z, node.normal.x,
                         node.normal.y, node.normal.z});
  out.push_back(PackU8x4(vb.layers[0], vb.layers[1], vb.layers[2], vb.layers[3]));
  out.push_back(
      PackU8x4(vb.weights[0], vb.weights[1], vb.weights[2], vb.weights[3]));
}

}  // namespace

VertexBlend ResolveVertexBlend(const BiomeWeights& weights) {
  VertexBlend out;  // layers default to 0, weights to 0 (the padding contract)

  // Rank the biomes by weight. stable_sort on an index list keeps ties in
  // ascending biome order, so selection is deterministic.
  std::array<int, kBiomeSliceCount> order{};
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(),
                   [&](int a, int b) { return weights.w[a] > weights.w[b]; });

  // Keep the 4 strongest with actual coverage.
  std::array<int, 4> keep{};
  std::array<float, 4> kept_w{};
  int n = 0;
  for (int k = 0; k < kBiomeSliceCount && n < 4; ++k) {
    const int b = order[k];
    if (weights.w[b] <= 0.0f) break;  // sorted desc: nothing after this matters
    keep[n] = b;
    kept_w[n] = weights.w[b];
    ++n;
  }
  if (n == 0) return out;  // no coverage: all-zero weights, never NaN

  float sum = 0.0f;
  for (int k = 0; k < n; ++k) sum += kept_w[k];
  if (sum <= 0.0f) return out;

  // Sum-preserving (largest-remainder) quantization so the packed weights total
  // exactly 255 -- removes the drift naive per-slot rounding would leave.
  std::array<int, 4> q{};
  std::array<float, 4> frac{};
  int total = 0;
  for (int k = 0; k < n; ++k) {
    const float exact = (kept_w[k] / sum) * 255.0f;
    q[k] = static_cast<int>(std::floor(exact));
    frac[k] = exact - static_cast<float>(q[k]);
    total += q[k];
  }
  for (int remainder = 255 - total; remainder > 0; --remainder) {
    int best = 0;
    for (int k = 1; k < n; ++k) {
      if (frac[k] > frac[best]) best = k;
    }
    q[best] += 1;
    frac[best] = -1.0f;  // consumed
  }

  for (int k = 0; k < n; ++k) {
    out.layers[k] = static_cast<uint8_t>(keep[k]);
    out.weights[k] = static_cast<uint8_t>(std::clamp(q[k], 0, 255));
  }
  return out;
}

bool RaycastTerrain(const MapData& map, const Ray& ray, glm::vec3& out_hit) {
  if (map.empty()) return false;
  const float step = map.spacing_m() * 0.5f;
  const float max_t = 4.0f * std::max(map.size_x_m(), map.size_z_m());

  auto below = [&](const glm::vec3& p) {
    return p.y <= map.HeightAt(p.x, p.z);
  };

  // A ray that STARTS underground never crosses the surface going forward, so
  // there is nothing to pick (a camera below the terrain must not hover-hit).
  glm::vec3 prev = ray.origin;
  if (below(prev)) return false;

  for (float t = step; t <= max_t; t += step) {
    const glm::vec3 cur = ray.At(t);
    if (below(cur)) {
      // Bisect between prev (above) and cur (below).
      glm::vec3 lo = prev, hi = cur;
      for (int k = 0; k < 24; ++k) {
        const glm::vec3 mid = (lo + hi) * 0.5f;
        if (below(mid)) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      out_hit = (lo + hi) * 0.5f;
      out_hit.y = map.HeightAt(out_hit.x, out_hit.z);
      return true;
    }
    prev = cur;
  }
  return false;
}

TerrainMesh BuildTerrainMesh(const MapData& map,
                             const TerrainMeshParams& params) {
  TerrainMesh mesh;
  if (map.empty()) return mesh;

  const int cells_total_x = map.nodes_x() - 1;
  const int cells_total_z = map.nodes_z() - 1;
  if (cells_total_x <= 0 || cells_total_z <= 0) return mesh;

  const int cx0 = std::clamp(params.cell_x0, 0, cells_total_x);
  const int cz0 = std::clamp(params.cell_z0, 0, cells_total_z);
  const int cw = params.cells_x < 0
                     ? cells_total_x - cx0
                     : std::min(params.cells_x, cells_total_x - cx0);
  const int chh = params.cells_z < 0
                      ? cells_total_z - cz0
                      : std::min(params.cells_z, cells_total_z - cz0);
  if (cw <= 0 || chh <= 0) return mesh;

  const int corner_cols = cw + 1;
  const int corner_rows = chh + 1;
  const int corner_count = corner_cols * corner_rows;

  // Corner nodes first (shared between cells), then one derived centre per cell.
  std::vector<Node> corners;
  corners.reserve(static_cast<std::size_t>(corner_count));
  for (int lz = 0; lz < corner_rows; ++lz)
    for (int lx = 0; lx < corner_cols; ++lx)
      corners.push_back(MakeCornerNode(map, cx0 + lx, cz0 + lz));

  auto corner_at = [&](int lx, int lz) -> const Node& {
    return corners[static_cast<std::size_t>(lz) * corner_cols + lx];
  };

  mesh.vertices.reserve(static_cast<std::size_t>(corner_count + cw * chh) *
                        TerrainMesh::kFloatsPerVertex);
  for (const Node& n : corners) EmitVertex(mesh.vertices, n);
  for (int lz = 0; lz < chh; ++lz) {
    for (int lx = 0; lx < cw; ++lx) {
      EmitVertex(mesh.vertices,
                 AverageNodes(corner_at(lx, lz), corner_at(lx + 1, lz),
                              corner_at(lx + 1, lz + 1), corner_at(lx, lz + 1)));
    }
  }

  auto corner_idx = [&](int lx, int lz) {
    return static_cast<uint32_t>(lz * corner_cols + lx);
  };
  auto center_idx = [&](int lx, int lz) {
    return static_cast<uint32_t>(corner_count + lz * cw + lx);
  };

  mesh.indices.reserve(static_cast<std::size_t>(cw) * chh * 4 * 3);
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  };
  for (int lz = 0; lz < chh; ++lz) {
    for (int lx = 0; lx < cw; ++lx) {
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

  mesh.vertex_count =
      static_cast<uint32_t>(mesh.vertices.size() / TerrainMesh::kFloatsPerVertex);
  return mesh;
}

}  // namespace badlands
