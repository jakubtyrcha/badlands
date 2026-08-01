#include "game/geometry/terrain_clusters.hpp"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <meshoptimizer.h>
#include <spdlog/spdlog.h>

#include "core/parallel.hpp"  // ParallelFor (per-group build parallelism)
#include "mapgen/biomes.hpp"

namespace badlands {

namespace {

using mapgen::kBiomePalette;

// Pack four u8 into one float slot (matches the Uint8x4 / Unorm8x4 vertex
// attributes). Local copy: the MapData-based terrain_mesh keeps its own
// PackU8x4 internal, and the cluster build has no other terrain_mesh dependency.
float PackU8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint32_t u = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
                     (static_cast<uint32_t>(c) << 16) |
                     (static_cast<uint32_t>(d) << 24);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

// Surface normal from central differences of the map's bilinear heightmap, one
// lattice spacing apart. A pure function of world position (HeightAt clamps at
// the edges), so two callers sampling the same node get bitwise-identical
// normals -- what makes shared boundary vertices crack-free. Mirrors the simple
// terrain_mesh builder's normal so both agree on the surface.
glm::vec3 NormalAt(const MapData& map, float wx, float wz) {
  const float d = map.spacing_m();
  const float hl = map.HeightAt(wx - d, wz);
  const float hr = map.HeightAt(wx + d, wz);
  const float hd = map.HeightAt(wx, wz - d);
  const float hu = map.HeightAt(wx, wz + d);
  return glm::normalize(glm::vec3(-(hr - hl) / (2.0f * d), 1.0f,
                                  -(hu - hd) / (2.0f * d)));
}

// A leaf quad is 2 triangles, so a tile_quads^2 tile is this many triangles —
// the per-cluster budget (mirrors kClusterTriBudget for the default tile_quads).
constexpr int kTrisPerQuad = 2;
// A group whose simplified output already fits within this multiple of the
// budget collapses to a single parent instead of splitting (trailing/cheap
// groups); the spec's "~1.5x budget" heuristic.
constexpr float kSmallGroupBudgetFactor = 1.5f;

// A build-time vertex. Position + the leaf-derived normal + biome id; color is
// re-derived from the biome palette at pack time so its bytes stay exact and
// bitwise-stable across clusters (crack-freeness). Normal is CARRIED unchanged
// from the leaf (never recomputed), so a shared boundary vertex is bitwise-equal
// wherever it appears.
struct WorkVertex {
  glm::vec3 pos;
  glm::vec3 normal;
  uint8_t biome;
};

struct ClusterGeom {
  std::vector<WorkVertex> verts;
  std::vector<uint32_t> tris;  // 3 vertex indices per triangle
};

// A region tiles a rectangle of the map (world XZ) and holds the clusters that
// currently cover it. The build reduces a grid of regions each level.
struct Region {
  float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  std::vector<uint32_t> clusters;
};

struct RegionGrid {
  int nx = 0, nz = 0;
  std::vector<Region> cells;
  Region& at(int rx, int rz) { return cells[static_cast<size_t>(rz) * nx + rx]; }
  const Region& at(int rx, int rz) const {
    return cells[static_cast<size_t>(rz) * nx + rx];
  }
};

// Debug-tint hash, a pure function of world position so a shared boundary vertex
// gets the same byte in every cluster that carries it (required for the
// crack-free full-record equality — the meta byte is part of the compared 8
// floats). Per-location rather than strictly per-cluster; that is a debug
// nicety, not a correctness property.
uint8_t ClusterHashByte(const glm::vec3& p) {
  uint32_t b[3];
  std::memcpy(b, &p, sizeof(b));
  uint32_t hsh = b[0] * 73856093u ^ b[1] * 19349663u ^ b[2] * 83492791u;
  hsh ^= hsh >> 15;
  hsh *= 2246822519u;
  hsh ^= hsh >> 13;
  return static_cast<uint8_t>(hsh & 0xFFu);
}

// Bit key for exact-float-position welding (meshopt never moves vertices and all
// positions descend from the same SampleHeight evaluations, so equal positions
// are bitwise-equal).
struct PosKey {
  uint32_t x, y, z;
  bool operator==(const PosKey& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};
struct PosKeyHash {
  size_t operator()(const PosKey& k) const {
    size_t h = k.x * 73856093u;
    h ^= k.y * 19349663u + (h << 6) + (h >> 2);
    h ^= k.z * 83492791u + (h << 6) + (h >> 2);
    return h;
  }
};
PosKey KeyOf(const glm::vec3& p) {
  PosKey k;
  std::memcpy(&k.x, &p.x, 4);
  std::memcpy(&k.y, &p.y, 4);
  std::memcpy(&k.z, &p.z, 4);
  return k;
}

// Pack a cluster's geometry into the DAG's shared buffers, appending the cluster
// record + its ClusterGeom (kept for the next level to consume). Returns the new
// cluster index.
uint32_t EmitCluster(TerrainClusterDag& dag,
                     std::vector<ClusterGeom>& cluster_geom, ClusterGeom geom,
                     int level, int own_group) {
  TerrainCluster c;
  c.first_vertex = static_cast<uint32_t>(dag.vertices.size() /
                                         kFloatsPerClusterVertex);
  c.vertex_count = static_cast<uint32_t>(geom.verts.size());
  c.first_index = static_cast<uint32_t>(dag.indices.size());
  c.index_count = static_cast<uint32_t>(geom.tris.size());
  c.level = level;
  c.own_group = own_group;

  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  for (const WorkVertex& v : geom.verts) {
    // Palette bytes are written RAW (no sRGB->linear decode), matching the
    // engine terrain_blend material: the G-buffer albedo target is linear
    // BGRA8Unorm and terrain_blend loads its textures as plain RGBA8Unorm (no
    // sRGB decode), so both treat 8-bit albedo as linear. Linearizing here would
    // make cluster terrain too dark. See the M4 color-path finding in the docs.
    const mapgen::Rgb col = kBiomePalette[v.biome];
    dag.vertices.push_back(v.pos.x);
    dag.vertices.push_back(v.pos.y);
    dag.vertices.push_back(v.pos.z);
    dag.vertices.push_back(v.normal.x);
    dag.vertices.push_back(v.normal.y);
    dag.vertices.push_back(v.normal.z);
    dag.vertices.push_back(PackU8x4(col.r, col.g, col.b, 255));
    dag.vertices.push_back(PackU8x4(v.biome, ClusterHashByte(v.pos),
                                    static_cast<uint8_t>(level), 0));
    lo = glm::min(lo, v.pos);
    hi = glm::max(hi, v.pos);
  }
  for (uint32_t idx : geom.tris) dag.indices.push_back(c.first_vertex + idx);
  c.bounds = Aabb::FromMinMax(lo, hi);

  const uint32_t cidx = static_cast<uint32_t>(dag.clusters.size());
  dag.clusters.push_back(c);
  cluster_geom.push_back(std::move(geom));
  return cidx;
}

// The 2-triangle-per-quad leaf tile [qx0,qx1] x [qz0,qz1] (in lattice nodes),
// vertex grid at map.spacing_m() spacing, one consistent diagonal (n00->n11).
// Height/normal/biome sample the frozen MapData lattice directly.
ClusterGeom BuildLeafGeom(const MapData& map, int qx0, int qz0, int qx1,
                          int qz1) {
  const float sp = map.spacing_m();
  const int vx = qx1 - qx0 + 1;
  const int vz = qz1 - qz0 + 1;
  ClusterGeom g;
  g.verts.reserve(static_cast<size_t>(vx) * vz);
  for (int j = qz0; j <= qz1; ++j) {
    for (int i = qx0; i <= qx1; ++i) {
      const float wx = static_cast<float>(i) * sp;
      const float wz = static_cast<float>(j) * sp;
      WorkVertex v;
      v.pos = glm::vec3(wx, map.height(i, j), wz);
      v.normal = NormalAt(map, wx, wz);
      v.biome = static_cast<uint8_t>(map.WeightsAtNode(i, j).Dominant());
      g.verts.push_back(v);
    }
  }
  auto vid = [&](int li, int lj) {
    return static_cast<uint32_t>(lj * vx + li);
  };
  g.tris.reserve(static_cast<size_t>(vx - 1) * (vz - 1) * 6);
  for (int lj = 0; lj < vz - 1; ++lj) {
    for (int li = 0; li < vx - 1; ++li) {
      const uint32_t n00 = vid(li, lj), n10 = vid(li + 1, lj);
      const uint32_t n01 = vid(li, lj + 1), n11 = vid(li + 1, lj + 1);
      // CCW winding as seen from above (+Y up): geometric normal points +Y, so
      // the tris are front faces under the renderer's CCW/back-cull convention.
      g.tris.insert(g.tris.end(), {n00, n11, n10});
      g.tris.insert(g.tris.end(), {n00, n01, n11});
    }
  }
  return g;
}

// --- local subdivision (TerrainDetailField) ---------------------------------
//
// EXACT ARITHMETIC IS LOAD-BEARING here: WeldChildren and LockVertex compare
// positions with ==, and the same physical vertex is emitted by up to four
// quads at DIFFERENT subdivision factors. Every world coordinate below is one
// IEEE multiplication (integer fine index) * (dyadic step), both factors exact,
// so each expression is the correctly-rounded value of the same real number --
// (q*8 + 2t) * (sp/8) and (q*4 + t) * (sp/4) land on the identical float. Any
// other factorization (q*sp + t*step) rounds twice and breaks the weld.

// Validated, clamped view of a TerrainDetailField. `kmax` derives from the
// CLUSTER BUDGET (2*4^k <= budget), not from any use case.
struct DetailView {
  const TerrainDetailField* field = nullptr;  // null = no detail anywhere
  int kmax = 0;

  int QuadExp(int qx, int qz) const {
    if (!field) return 0;
    if (qx < 0 || qz < 0 || qx >= field->width || qz >= field->height) return 0;
    const int k = field->level[static_cast<size_t>(qz) * field->width + qx];
    return std::min(k, kmax);
  }
};

DetailView MakeDetailView(const TerrainDetailField* detail, int W, int H,
                          int budget) {
  DetailView v;
  if (!detail || !detail->level) return v;
  if (detail->width != W || detail->height != H) {
    spdlog::warn(
        "TerrainDetailField ignored: exponent grid {}x{} does not match the "
        "map quad grid {}x{}",
        detail->width, detail->height, W, H);
    return v;
  }
  if (!detail->height_at) {
    spdlog::warn("TerrainDetailField ignored: no height_at sampler");
    return v;
  }
  v.field = detail;
  while (2 * (1 << (2 * (v.kmax + 1))) <= budget) ++v.kmax;
  return v;
}

// Central-difference normal of the DETAIL surface. Same shape as NormalAt, but
// against height_at and at spacing `d` -- the coarse NormalAt would shade a
// carved channel flat. `d` must be SEAM-CANONICAL: every emitter of the same
// vertex must pass the same d (interior verts use the quad's own step, on-edge
// verts the shared edge's step), or the weld breaks on the normal.
glm::vec3 DetailNormalAt(const TerrainDetailField& f, float wx, float wz,
                         float d) {
  const float hl = f.height_at(wx - d, wz);
  const float hr = f.height_at(wx + d, wz);
  const float hd = f.height_at(wx, wz - d);
  const float hu = f.height_at(wx, wz + d);
  return glm::normalize(glm::vec3(-(hr - hl) / (2.0f * d), 1.0f,
                                  -(hu - hd) / (2.0f * d)));
}

// A base-lattice node's record under detail. The rule is decided by the FOUR
// quads around the node, which every emitter can look up identically:
//
//   - any of them plain (or off-map)  -> the COARSE record (map height,
//     NormalAt, node biome), bitwise what BuildLeafGeom emits -- because a
//     plain quad WILL emit this node and the two copies must weld. The
//     producer's compact-support contract makes this non-lossy: a node
//     touching an unrefined quad is outside the detail's support.
//   - all four refined -> the DETAIL record, or the node would pin an uncarved
//     post into the middle of every channel. The differencing step is
//     canonical over the same four exponents (the minimum), so all four
//     emitters agree bitwise.
WorkVertex LatticeNodeVertex(const MapData& map, const DetailView& dv, int i,
                             int j) {
  const float sp = map.spacing_m();
  const float wx = static_cast<float>(i) * sp;
  const float wz = static_cast<float>(j) * sp;
  WorkVertex v;
  v.biome = static_cast<uint8_t>(map.WeightsAtNode(i, j).Dominant());
  const int kmin =
      std::min(std::min(dv.QuadExp(i - 1, j - 1), dv.QuadExp(i, j - 1)),
               std::min(dv.QuadExp(i - 1, j), dv.QuadExp(i, j)));
  if (kmin == 0) {
    v.pos = glm::vec3(wx, map.height(i, j), wz);
    v.normal = NormalAt(map, wx, wz);
    return v;
  }
  const float d = sp / static_cast<float>(1 << kmin);
  v.pos = glm::vec3(wx, dv.field->height_at(wx, wz), wz);
  v.normal = DetailNormalAt(*dv.field, wx, wz, d);
  return v;
}

// One vertex of a refined quad, by GLOBAL fine index (gx, gz) at factor f.
// Base-lattice nodes (both indices multiples of f) route through
// LatticeNodeVertex; everything else samples the detail surface, with
// `normal_d` the seam-canonical differencing step (the quad's own step for
// interior vertices, the shared edge's step for on-edge vertices).
WorkVertex DetailVertex(const MapData& map, const DetailView& dv, int f, int gx,
                        int gz, float normal_d) {
  if (gx % f == 0 && gz % f == 0)
    return LatticeNodeVertex(map, dv, gx / f, gz / f);
  const TerrainDetailField& field = *dv.field;
  const float sp = map.spacing_m();
  const float step = sp / static_cast<float>(f);
  const float wx = static_cast<float>(gx) * step;
  const float wz = static_cast<float>(gz) * step;
  WorkVertex v;
  v.pos = glm::vec3(wx, field.height_at(wx, wz), wz);
  v.normal = DetailNormalAt(field, wx, wz, normal_d);
  v.biome = static_cast<uint8_t>(map.BiomesAt(wx, wz).Dominant());
  return v;
}

// Appends one quad's triangles to `g`, welding vertices through `weld` (keyed
// by exact position). Plain quads (k == 0) emit exactly BuildLeafGeom's two
// triangles -- same records, same diagonal -- so a hot tile's plain remainder
// is indistinguishable from a cold tile's geometry.
//
// A refined quad is (a) its full-resolution interior grid, inset one fine step
// from the quad boundary, plus (b) ONE ring strip joining two closed loops:
// the inset grid's perimeter (always at the quad's own resolution) and the
// quad boundary (each edge at ITS OWN resolution, 2^min(ka, kb) against that
// edge's neighbour). The two loops are walked in lockstep by normalized
// perimeter arc length, emitting one triangle per step -- equal-resolution
// edges, fanned edges, and the corners where they meet all fall out of the
// same walk, so there is no corner case to enumerate (or get wrong).
void AppendQuadGeom(ClusterGeom& g,
                    std::unordered_map<PosKey, uint32_t, PosKeyHash>& weld,
                    const MapData& map, const DetailView& dv, int qx, int qz) {
  const float sp = map.spacing_m();
  auto add = [&](const WorkVertex& v) -> uint32_t {
    const PosKey key = KeyOf(v.pos);
    auto it = weld.find(key);
    if (it != weld.end()) return it->second;
    const uint32_t idx = static_cast<uint32_t>(g.verts.size());
    g.verts.push_back(v);
    weld.emplace(key, idx);
    return idx;
  };
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    g.tris.insert(g.tris.end(), {a, b, c});
  };

  const int k = dv.QuadExp(qx, qz);
  if (k == 0) {
    // BuildLeafGeom's quad, record for record: a plain quad forces kmin == 0
    // at all four of its nodes, so LatticeNodeVertex yields exactly the coarse
    // records. Same diagonal n00 -> n11, CCW seen from +Y.
    auto corner = [&](int i, int j) {
      return add(LatticeNodeVertex(map, dv, i, j));
    };
    const uint32_t n00 = corner(qx, qz), n10 = corner(qx + 1, qz);
    const uint32_t n01 = corner(qx, qz + 1), n11 = corner(qx + 1, qz + 1);
    tri(n00, n11, n10);
    tri(n00, n01, n11);
    return;
  }

  const int f = 1 << k;
  const float own_d = sp / static_cast<float>(f);

  // Interior grid: vertices (1..f-1)^2 in quad-local fine coords, faces over
  // the (f-2)^2 inner cells. Same diagonal and winding as the coarse quad.
  auto interior = [&](int li, int lj) {
    return add(DetailVertex(map, dv, f, qx * f + li, qz * f + lj, own_d));
  };
  for (int lj = 1; lj + 1 <= f - 1; ++lj) {
    for (int li = 1; li + 1 <= f - 1; ++li) {
      const uint32_t n00 = interior(li, lj), n10 = interior(li + 1, lj);
      const uint32_t n01 = interior(li, lj + 1), n11 = interior(li + 1, lj + 1);
      tri(n00, n11, n10);
      tri(n00, n01, n11);
    }
  }

  // The two ring loops, both traversed (0,0) -> (0,f) -> (f,f) -> (f,0) in
  // quad-local fine coords -- the orientation that makes the emitted strip
  // face +Y (verified by the winding test). Each loop vertex carries its
  // normalized perimeter position for the lockstep walk.
  struct LoopVert {
    uint32_t idx;
    float param;
  };

  // Outer loop: the quad boundary. Edge order west/north/east/south to match
  // the traversal above; each edge at 2^min(k, neighbour k). On-edge non-corner
  // vertices difference at the EDGE's step (seam-canonical: the neighbour quad
  // computes the same fe from the same pair of exponents).
  std::vector<LoopVert> outer;
  {
    struct Edge {
      int nqx, nqz;        // the neighbour that shares it
      int ox, oz, dx, dz;  // start corner and direction, quad-local units
    };
    const Edge edges[4] = {
        {qx - 1, qz, 0, 0, 0, 1},  // west:  (0,0) -> (0,f)
        {qx, qz + 1, 0, 1, 1, 0},  // north: (0,f) -> (f,f)
        {qx + 1, qz, 1, 1, 0, -1}, // east:  (f,f) -> (f,0)
        {qx, qz - 1, 1, 0, -1, 0}, // south: (f,0) -> (0,0)
    };
    for (int e = 0; e < 4; ++e) {
      const int fe = 1 << std::min(k, dv.QuadExp(edges[e].nqx, edges[e].nqz));
      const int stride = f / fe;  // fine steps between edge vertices
      const float edge_d = sp / static_cast<float>(fe);
      for (int t = 0; t < f; t += stride) {
        const int li = edges[e].ox * f + edges[e].dx * t;
        const int lj = edges[e].oz * f + edges[e].dz * t;
        const uint32_t idx = add(DetailVertex(map, dv, f, qx * f + li,
                                              qz * f + lj, edge_d));
        outer.push_back(
            {idx, (static_cast<float>(e) + static_cast<float>(t) /
                                               static_cast<float>(f)) *
                      0.25f});
      }
    }
  }

  // Inner loop: the inset grid's perimeter, same rotational sense. Degenerates
  // to a single vertex at f == 2, which turns the walk into a plain fan.
  std::vector<LoopVert> inner;
  if (f == 2) {
    inner.push_back({interior(1, 1), 0.0f});
  } else {
    const int m = f - 2;  // segments per inner edge
    const int corners[4][4] = {
        {1, 1, 0, 1},          // west:  (1,1) -> (1,f-1)
        {1, f - 1, 1, 0},      // north
        {f - 1, f - 1, 0, -1}, // east
        {f - 1, 1, -1, 0},     // south
    };
    for (int e = 0; e < 4; ++e) {
      for (int t = 0; t < m; ++t) {
        const int li = corners[e][0] + corners[e][2] * t;
        const int lj = corners[e][1] + corners[e][3] * t;
        inner.push_back(
            {interior(li, lj), (static_cast<float>(e) + static_cast<float>(t) /
                                                            static_cast<float>(m)) *
                                   0.25f});
      }
    }
  }

  // Lockstep walk: advance whichever loop's NEXT vertex sits earlier along the
  // perimeter (ties to the outer), one triangle per advance. No + Ni triangles;
  // with every neighbour at the same exponent that is exactly the 2*f^2 - the
  // interior faces of a full grid, so nothing is lost to the ring.
  const size_t no = outer.size(), ni = inner.size();
  size_t o = 0, i = (ni == 1) ? 1 : 0;
  while (o < no || i < ni) {
    const float next_o = (o + 1 < no) ? outer[o + 1].param : 1.0f;
    const float next_i = (i + 1 < ni) ? inner[i + 1].param : 1.0f;
    const bool adv_outer = (i >= ni) || (o < no && next_o <= next_i);
    if (adv_outer) {
      tri(outer[o].idx, outer[(o + 1) % no].idx, inner[i % ni].idx);
      ++o;
    } else {
      tri(outer[o % no].idx, inner[(i + 1) % ni].idx, inner[i].idx);
      ++i;
    }
  }
}

// The leaf clusters of a tile CONTAINING refined quads: one cluster per cell
// of a per-tile SUB-GRID, sized so a cell can never exceed the triangle
// budget. Cell edge = tile_quads >> kmax quads: a cell of side s holds at most
// s^2 * 2*4^kmax = 2*tile_quads^2 triangles -- exactly the budget -- and plain
// quads only reduce it.
//
// The cells being PROPER RECTANGLES is load-bearing, not aesthetic: the
// reduction rounds (ReduceGrid) lock group boundaries by rectangular
// footprint, so a cell grid is what lets the pre-pass reduce a tile with the
// SAME machinery the map-level merge uses. Row-major packing cut at the budget
// would be denser, but its cluster boundaries are not rectangles and nothing
// could lock them.
struct TileCells {
  int nx = 0, nz = 0;  // cell-grid dims over this tile (all cells non-empty)
  int side = 1;        // quads per cell edge
  std::vector<ClusterGeom> geoms;  // row-major, one per cell
};

TileCells BuildDetailedTileCells(const MapData& map, const DetailView& dv,
                                 int qx0, int qz0, int qx1, int qz1, int Q) {
  int kmax = 0;
  for (int qz = qz0; qz < qz1; ++qz)
    for (int qx = qx0; qx < qx1; ++qx)
      kmax = std::max(kmax, dv.QuadExp(qx, qz));
  TileCells out;
  out.side = std::max(1, Q >> kmax);
  // Dims from the tile's ACTUAL quad extent (a border tile can be partial), so
  // every cell holds at least one quad -- ReduceGrid must never see an empty
  // region.
  out.nx = (qx1 - qx0 + out.side - 1) / out.side;
  out.nz = (qz1 - qz0 + out.side - 1) / out.side;
  out.geoms.reserve(static_cast<size_t>(out.nx) * out.nz);
  for (int cz = 0; cz < out.nz; ++cz) {
    for (int cx = 0; cx < out.nx; ++cx) {
      ClusterGeom g;
      std::unordered_map<PosKey, uint32_t, PosKeyHash> weld;
      const int ax0 = qx0 + cx * out.side;
      const int ax1 = std::min(ax0 + out.side, qx1);
      const int az0 = qz0 + cz * out.side;
      const int az1 = std::min(az0 + out.side, qz1);
      for (int qz = az0; qz < az1; ++qz)
        for (int qx = ax0; qx < ax1; ++qx)
          AppendQuadGeom(g, weld, map, dv, qx, qz);
      out.geoms.push_back(std::move(g));
    }
  }
  return out;
}

// Weld the children's geometry by exact float position. Attributes come from the
// first occurrence (all copies of a shared vertex are bitwise-identical).
ClusterGeom WeldChildren(const std::vector<ClusterGeom>& cluster_geom,
                         const std::vector<uint32_t>& children) {
  ClusterGeom merged;
  std::unordered_map<PosKey, uint32_t, PosKeyHash> weld;
  for (uint32_t cidx : children) {
    const ClusterGeom& cg = cluster_geom[cidx];
    std::vector<uint32_t> remap(cg.verts.size());
    for (size_t v = 0; v < cg.verts.size(); ++v) {
      const PosKey k = KeyOf(cg.verts[v].pos);
      auto it = weld.find(k);
      if (it == weld.end()) {
        const uint32_t nv = static_cast<uint32_t>(merged.verts.size());
        merged.verts.push_back(cg.verts[v]);
        weld.emplace(k, nv);
        remap[v] = nv;
      } else {
        remap[v] = it->second;
      }
    }
    for (uint32_t idx : cg.tris) merged.tris.push_back(remap[idx]);
  }
  return merged;
}

// Result of processing one group, computed with no shared mutable state (ready
// for a future ParallelFor over groups within a level).
struct GroupResult {
  std::vector<ClusterGeom> outputs;
  float result_error = 0.0f;
};

// Group footprint edges that touch ANOTHER group get locked (exact float
// compare of x/z against the footprint edge coords — small integers in float).
// Map-perimeter edges (coord 0 or the map extent) have no neighbour, so they
// stay unlocked; a perimeter vertex is still locked if it also sits on a
// perpendicular interior seam.
unsigned char LockVertex(const glm::vec3& p, const glm::vec4& fp, float map_w,
                         float map_h) {
  const bool on = (p.x == fp.x && fp.x > 0.0f) ||       // left, interior
                  (p.x == fp.z && fp.z < map_w) ||       // right, interior
                  (p.z == fp.y && fp.y > 0.0f) ||        // bottom, interior
                  (p.z == fp.w && fp.w < map_h);         // top, interior
  return on ? 1 : 0;
}

// Simplify the welded mesh under the locked boundary, then split it into
// contiguous median cuts along the longer footprint axis. footprint = {x0, z0,
// x1, z1}.
GroupResult SimplifyAndSplit(ClusterGeom merged, const glm::vec4& footprint,
                             float map_w, float map_h,
                             const TerrainClusterParams& params,
                             bool collapse_to_one = false) {
  const size_t vcount = merged.verts.size();
  std::vector<float> positions(vcount * 3);
  std::vector<float> attrs(vcount * 6);
  std::vector<unsigned char> lock(vcount);
  for (size_t v = 0; v < vcount; ++v) {
    const WorkVertex& w = merged.verts[v];
    positions[v * 3 + 0] = w.pos.x;
    positions[v * 3 + 1] = w.pos.y;
    positions[v * 3 + 2] = w.pos.z;
    const mapgen::Rgb col = kBiomePalette[w.biome];
    attrs[v * 6 + 0] = w.normal.x;
    attrs[v * 6 + 1] = w.normal.y;
    attrs[v * 6 + 2] = w.normal.z;
    attrs[v * 6 + 3] = col.r / 255.0f;
    attrs[v * 6 + 4] = col.g / 255.0f;
    attrs[v * 6 + 5] = col.b / 255.0f;
    lock[v] = LockVertex(w.pos, footprint, map_w, map_h);
  }
  const float weights[6] = {
      params.attr_weight_normal, params.attr_weight_normal,
      params.attr_weight_normal, params.attr_weight_color,
      params.attr_weight_color,  params.attr_weight_color};

  const size_t index_count = merged.tris.size();
  size_t target = static_cast<size_t>(index_count * params.simplify_target_ratio);
  target -= target % 3;
  if (target < 3) target = std::min<size_t>(index_count, 3);

  // The locked border is a FLOOR on reduction, and asking for less than it can
  // support is how degenerate geometry is born: a patch whose border carries L
  // locked vertices cannot honestly go below ~L-2 triangles, and given a
  // smaller target meshopt keeps the locked vertices alive inside zero-area
  // "wall" triangles ON the border line -- invisible to the error metric,
  // 4-triangle edges in the cut, and a mask over real T-junctions (a walled
  // vertex is dropped from the border polyline in every way that matters).
  // Observed on tiny pre-pass groups (2-8 tris with several locked verts)
  // before this floor existed; uniform-build groups sit far above it, so their
  // output is unchanged.
  size_t locked = 0;
  for (unsigned char l : lock) locked += l;
  const size_t floor_indices = 3 * (locked > 2 ? locked - 2 : 1);
  if (target < floor_indices) target = std::min(index_count, floor_indices);

  std::vector<uint32_t> simplified(index_count);
  float result_error = 0.0f;
  const size_t got = meshopt_simplifyWithAttributes(
      simplified.data(), merged.tris.data(), index_count, positions.data(),
      vcount, sizeof(float) * 3, attrs.data(), sizeof(float) * 6, weights, 6,
      lock.data(), target, FLT_MAX, meshopt_SimplifyErrorAbsolute,
      &result_error);
  simplified.resize(got);

  GroupResult res;
  res.result_error = result_error;
  const size_t tri_count = got / 3;
  if (tri_count == 0) return res;

  // Output-cluster count: a full group splits into group_split_count; a small
  // group (trailing / already-cheap) collapses to a single parent.
  //
  // `collapse_to_one` (the TERMINAL round of a ReduceGrid, everything in one
  // group) overrides the size test, and is what makes termination PROVABLE
  // rather than usual: the locked-vertex simplify floor can hold a merge above
  // small_group_max forever -- two clusters weld to 258 locked-floored
  // triangles, split back into two, weld to the same 258, ad infinitum (a
  // measured livelock, ~50 MB/s of appended clusters). A terminal merge has no
  // one to split FOR, so it emits one cluster of whatever size the floor
  // dictates; the overshoot drains at the next level up, where the alternating
  // partition buries the locked seam inside a group. On a uniform build the
  // terminal merge already lands under small_group_max, so this is a no-op
  // there (bit-identical, pinned by the determinism suite).
  const int budget = kTrisPerQuad * params.tile_quads * params.tile_quads;
  const int small_group_max =
      static_cast<int>(budget * kSmallGroupBudgetFactor);
  int num_out = (collapse_to_one || static_cast<int>(tri_count) <= small_group_max)
                    ? 1
                    : params.group_split_count;
  num_out = std::min<int>(num_out, static_cast<int>(tri_count));
  num_out = std::max(1, num_out);

  // Sort triangles by centroid along the longer footprint axis, cut at medians.
  const bool split_x = (footprint.z - footprint.x) >= (footprint.w - footprint.y);
  std::vector<uint32_t> order(tri_count);
  std::iota(order.begin(), order.end(), 0u);
  auto centroid = [&](uint32_t t) {
    const glm::vec3& a = merged.verts[simplified[t * 3 + 0]].pos;
    const glm::vec3& b = merged.verts[simplified[t * 3 + 1]].pos;
    const glm::vec3& c = merged.verts[simplified[t * 3 + 2]].pos;
    const glm::vec3 m = (a + b + c) / 3.0f;
    return split_x ? m.x : m.z;
  };
  std::stable_sort(order.begin(), order.end(),
                   [&](uint32_t l, uint32_t r) { return centroid(l) < centroid(r); });

  res.outputs.resize(num_out);
  for (int o = 0; o < num_out; ++o) {
    const size_t lo = tri_count * o / num_out;
    const size_t hi = tri_count * (o + 1) / num_out;
    ClusterGeom& out = res.outputs[o];
    std::unordered_map<uint32_t, uint32_t> compact;  // welded idx -> local idx
    for (size_t s = lo; s < hi; ++s) {
      const uint32_t t = order[s];
      for (int k = 0; k < 3; ++k) {
        const uint32_t wi = simplified[t * 3 + k];
        auto it = compact.find(wi);
        uint32_t local;
        if (it == compact.end()) {
          local = static_cast<uint32_t>(out.verts.size());
          out.verts.push_back(merged.verts[wi]);
          compact.emplace(wi, local);
        } else {
          local = it->second;
        }
        out.tris.push_back(local);
      }
    }
  }
  return res;
}

// Bounding sphere of an AABB (center + half-diagonal). Aabb::Extents() is the
// full size (max-min), so half its length is the half-diagonal — bit-identical
// to the previous 0.5*length(max-min).
glm::vec4 SphereOfAabb(const Aabb& b) {
  return glm::vec4(b.Center(), 0.5f * glm::length(b.Extents()));
}

// Per-level build summary — the observable for M1 (nothing renders yet).
void LogStats(const TerrainClusterDag& dag, double build_ms, bool parallel,
              unsigned workers) {
  size_t total_tris = 0, total_verts = 0;
  spdlog::info("terrain cluster DAG: {} levels, {} clusters, {} groups",
               dag.level_count, dag.clusters.size(), dag.groups.size());
  for (int L = 0; L < dag.level_count; ++L) {
    int n = 0;
    uint32_t tmin = UINT32_MAX, tmax = 0;
    uint64_t tsum = 0;
    for (const TerrainCluster& c : dag.clusters) {
      if (c.level != L) continue;
      const uint32_t tris = c.index_count / 3;
      ++n;
      tmin = std::min(tmin, tris);
      tmax = std::max(tmax, tris);
      tsum += tris;
      total_tris += tris;
      total_verts += c.vertex_count;
    }
    float emin = 0.0f, emax = 0.0f;
    bool first = true;
    for (const TerrainClusterGroup& g : dag.groups) {
      if (g.level != L) continue;
      emin = first ? g.error_m : std::min(emin, g.error_m);
      emax = first ? g.error_m : std::max(emax, g.error_m);
      first = false;
    }
    if (n == 0) continue;
    spdlog::info(
        "  L{:<2} clusters={:<5} tris/cluster[min={} avg={} max={}] "
        "err_m[{:.3f}..{:.3f}]",
        L, n, tmin, tsum / static_cast<uint64_t>(n), tmax, emin, emax);
  }
  spdlog::info("  totals: {} tris, {} verts, build {:.1f} ms ({}, {} workers)",
               total_tris, total_verts, build_ms,
               parallel ? "parallel" : "serial", workers);
}

// Reduces `grid` to a single surviving cluster by repeated block -> weld ->
// boundary-locked-simplify -> split rounds (Phases A/B/C). THE machinery of
// the whole build: the map-level merge and the per-tile detail pre-pass both
// run exactly this -- the pre-pass is not a second code path, it is this one
// on a finer grid. Its trigger is purely structural (a grid holding more than
// one cluster); nothing here knows or may know what put the clusters there.
//
// `cur` counts ROUNDS and only shapes the blocking (first round pairs both
// axes, later rounds alternate one); levels derive from children at emission
// and the scratch free is by liveness, so the counter carries no tree meaning.
// Deterministic: Phase B parallelism never touches emission order.
// `prepass` marks the intra-region reduction rounds and unlocks ONE shortcut:
// a group whose welded input is already at or under the small-group threshold
// may PASS THROUGH unsimplified (one output, simplification error exactly 0).
// Reduction progress there is on CLUSTER count, not triangle count -- welding
// four 2-triangle plain cells and asking meshopt to halve them is pure
// overhead, and it was ~95% of the pre-pass's meshopt calls (measured 933 s of
// DAG build on the production corridor, dominated by meshopt on tiny groups).
// The MAIN merge never passes through, so a null-detail build -- which runs no
// pre-pass at all -- is bit-identical by construction, not by threshold luck.
void ReduceGrid(TerrainClusterDag& dag, std::vector<ClusterGeom>& cluster_geom,
                RegionGrid& grid, float map_w, float map_h,
                const TerrainClusterParams& params, bool prepass = false) {
  int cur = 0;
  auto grid_total = [&]() {
    size_t n = 0;
    for (const Region& r : grid.cells) n += r.clusters.size();
    return n;
  };
  const int regions_per_group =
      std::max(1, params.group_dim * params.group_dim / params.group_split_count);

  while (grid_total() > 1) {
    // Block shape over the region grid: level 0 pairs both axes (group_dim x
    // group_dim leaves = 4 children); higher levels pair a single alternating
    // axis (regions already hold group_split_count clusters each). Fall back to
    // the other axis when the alternating one can't reduce.
    int bx, bz;
    if (cur == 0) {
      bx = params.group_dim;
      bz = params.group_dim;
    } else if (grid.nx == 1 && grid.nz == 1) {
      bx = 1;
      bz = 1;  // final merge of the last region's clusters
    } else {
      bool pair_x = ((cur - 1) % 2) == 0;  // level 1 -> X, level 2 -> Z, ...
      if (pair_x && grid.nx == 1) pair_x = false;
      if (!pair_x && grid.nz == 1) pair_x = true;
      bx = pair_x ? regions_per_group : 1;
      bz = pair_x ? 1 : regions_per_group;
    }

    RegionGrid next;
    next.nx = (grid.nx + bx - 1) / bx;
    next.nz = (grid.nz + bz - 1) / bz;
    next.cells.resize(static_cast<size_t>(next.nx) * next.nz);
    const size_t group_count = next.cells.size();

    // Phase A (serial): per-output-cell child list + footprint. The flat index
    // g = orz*next.nx + orx makes the later serial emission run in the exact
    // orz-major/orx-minor order the single-loop build used, so cluster/group
    // ids and buffer offsets are assigned identically.
    struct GroupWork {
      std::vector<uint32_t> children;
      glm::vec4 footprint{0.0f};
    };
    std::vector<GroupWork> work(group_count);
    for (int orz = 0; orz < next.nz; ++orz) {
      for (int orx = 0; orx < next.nx; ++orx) {
        const int rx0 = orx * bx, rx1 = std::min(rx0 + bx, grid.nx);
        const int rz0 = orz * bz, rz1 = std::min(rz0 + bz, grid.nz);
        GroupWork& w = work[static_cast<size_t>(orz) * next.nx + orx];
        for (int rz = rz0; rz < rz1; ++rz)
          for (int rx = rx0; rx < rx1; ++rx)
            for (uint32_t c : grid.at(rx, rz).clusters) w.children.push_back(c);
        w.footprint = glm::vec4(grid.at(rx0, rz0).x0, grid.at(rx0, rz0).z0,
                                grid.at(rx1 - 1, rz1 - 1).x1,
                                grid.at(rx1 - 1, rz1 - 1).z1);
      }
    }

    // Phase B (parallel): weld + simplify + split each group into its own slot.
    // Both are pure functions of the immutable cluster_geom below this level and
    // the group's own inputs (meshopt is deterministic and thread-agnostic), so
    // distinct groups never touch shared mutable state and the per-slot results
    // are independent of scheduling — the determinism test pins parallel ==
    // serial byte-for-byte.
    // The 1x1 grid is the TERMINAL round -- one group, everything in it --
    // and it must collapse to a single cluster no matter what the locked
    // floor lets meshopt reach, or the loop never converges (see
    // SimplifyAndSplit's collapse_to_one).
    const bool terminal = grid.nx == 1 && grid.nz == 1;
    const int small_group_max = static_cast<int>(
        kTrisPerQuad * params.tile_quads * params.tile_quads *
        kSmallGroupBudgetFactor);
    std::vector<GroupResult> results(group_count);
    auto compute = [&](size_t g) {
      ClusterGeom merged = WeldChildren(cluster_geom, work[g].children);
      if (prepass &&
          static_cast<int>(merged.tris.size()) / 3 <= small_group_max) {
        // Pre-pass pass-through (see the function comment): the weld IS the
        // reduction, and an unsimplified output carries zero new error.
        results[g].result_error = 0.0f;
        results[g].outputs.clear();
        results[g].outputs.push_back(std::move(merged));
        return;
      }
      results[g] = SimplifyAndSplit(std::move(merged), work[g].footprint, map_w,
                                    map_h, params, terminal);
    };
    // Tiny rounds (per-tile pre-passes shrink to a handful of groups) pay more
    // in ParallelFor synchronization than the work is worth; run them inline.
    if (params.parallel_build && group_count >= 16) {
      ParallelFor(group_count, compute);
    } else {
      for (size_t g = 0; g < group_count; ++g) compute(g);
    }

    // Phase C (serial, deterministic): emit clusters/groups + build spheres in
    // the fixed g order. This is the only phase that mutates the DAG, so the
    // output layout is independent of how Phase B was scheduled.
    for (int orz = 0; orz < next.nz; ++orz) {
      for (int orx = 0; orx < next.nx; ++orx) {
        const size_t g = static_cast<size_t>(orz) * next.nx + orx;
        const std::vector<uint32_t>& children = work[g].children;
        const glm::vec4 footprint = work[g].footprint;
        GroupResult& gr = results[g];

        // The output level derives from the CHILDREN, not from the loop
        // counter: 1 + max(child.level). Identical on a uniform build (every
        // child sits at `cur`), but the loop counter stops being a tree
        // property the moment a region arrives pre-reduced at a deeper level
        // -- and from then on `level` is descriptive only (see the hpp).
        int out_level = 0;
        for (uint32_t c : children)
          out_level = std::max(out_level, dag.clusters[c].level + 1);

        // Reserve the group slot, emit its outputs (own_group = this group).
        const int gidx = static_cast<int>(dag.groups.size());
        dag.groups.emplace_back();
        std::vector<uint32_t> out_clusters;
        for (ClusterGeom& out : gr.outputs)
          out_clusters.push_back(EmitCluster(dag, cluster_geom, std::move(out),
                                             out_level, gidx));

        // Group record: monotone error + a sphere nesting the children spheres
        // and the outputs' AABB.
        TerrainClusterGroup& G = dag.groups[gidx];
        G.level = out_level;
        G.footprint = footprint;
        G.first_child = static_cast<uint32_t>(dag.group_children.size());
        G.child_count = static_cast<uint32_t>(children.size());
        float child_err = 0.0f;
        glm::vec3 center(0.0f);
        for (uint32_t c : children) {
          dag.group_children.push_back(c);
          dag.clusters[c].parent_group = gidx;
          child_err = std::max(child_err, dag.ClusterOwnError(dag.clusters[c]));
          center += glm::vec3(dag.ClusterOwnSphere(dag.clusters[c]));
        }
        center /= static_cast<float>(children.size());
        float radius = 0.0f;
        for (uint32_t c : children) {
          const glm::vec4 s = dag.ClusterOwnSphere(dag.clusters[c]);
          radius = std::max(radius, glm::length(center - glm::vec3(s)) + s.w);
        }
        for (uint32_t oc : out_clusters) {
          const Aabb& b = dag.clusters[oc].bounds;
          for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p((corner & 1) ? b.max.x : b.min.x,
                              (corner & 2) ? b.max.y : b.min.y,
                              (corner & 4) ? b.max.z : b.min.z);
            radius = std::max(radius, glm::length(center - p));
          }
        }
        G.error_m = std::max(gr.result_error, child_err);
        G.sphere = glm::vec4(center, radius);

        Region& nr = next.at(orx, orz);
        nr.x0 = footprint.x;
        nr.z0 = footprint.y;
        nr.x1 = footprint.z;
        nr.z1 = footprint.w;
        nr.clusters = std::move(out_clusters);
      }
    }
    grid = std::move(next);
    // Free the just-consumed scratch geometry BY LIVENESS, never by level:
    // exactly the clusters gathered as children this round are dead (welded
    // into their groups' outputs), and nothing else. The old form freed by
    // `level == cur`, which is the same set on a uniform build -- but on a
    // mixed-depth build a level-`cur` cluster can still be WAITING in a
    // region, and freeing it would silently weld an empty cluster later.
    // cluster_geom is parallel to dag.clusters and never serialized, so
    // clearing it cannot perturb the DAG output.
    for (const GroupWork& w : work)
      for (uint32_t c : w.children) cluster_geom[c] = ClusterGeom{};
    ++cur;
  }
}

}  // namespace

glm::vec4 TerrainClusterDag::ClusterOwnSphere(const TerrainCluster& c) const {
  if (c.own_group != kNoGroup) return groups[c.own_group].sphere;
  return SphereOfAabb(c.bounds);
}

void SelectClusters(const TerrainClusterDag& dag, glm::vec3 cam_pos,
                    float fov_deg, float screen_h_px, float tau_px,
                    std::vector<uint32_t>& out) {
  out.clear();

  // Sanitize tau into a finite window: negative/0/+inf/NaN each otherwise yield
  // an EMPTY cut (blank terrain) — see kMinTauPx/kMaxTauPx. Clamping guarantees a
  // valid non-empty exact cover for any caller input.
  tau_px = std::isnan(tau_px) ? kDefaultTauPx
                              : std::clamp(tau_px, kMinTauPx, kMaxTauPx);

  // Shared perspective scale: a world-meter error e at LOD-sphere distance d
  // projects to e * k / d pixels. k = screen_h / (2 tan(fov/2)).
  const float half_fov = glm::radians(fov_deg) * 0.5f;
  const float k = screen_h_px / (2.0f * std::tan(half_fov));
  constexpr float kEps = 1e-4f;  // guards the division at the sphere surface

  // Projected screen-space error of a cluster/group with LOD error `error_m`
  // and LOD bounding sphere `sphere` (xyz center, w radius). A zero-error
  // cluster (a leaf) projects to 0 from anywhere; a camera inside the sphere
  // yields +inf so the cut is forced to refine past it.
  auto proj = [&](float error_m, const glm::vec4& sphere) -> float {
    if (error_m <= 0.0f) return 0.0f;
    const float dist = glm::length(cam_pos - glm::vec3(sphere));
    if (dist <= sphere.w) return std::numeric_limits<float>::infinity();
    return error_m * k / std::max(dist - sphere.w, kEps);
  };

  // Flat pass over every cluster (no traversal, no frustum cull — the render
  // pass culls each range per-frustum). A cluster is on the cut iff its own
  // projected error is within budget but its parent group's is not: projected
  // error is monotone along any leaf->root chain (errors grow, parent spheres
  // enclose children), so the test fires on exactly one cluster per chain.
  out.reserve(dag.clusters.size() / 4 + 1);
  for (uint32_t i = 0; i < dag.clusters.size(); ++i) {
    const TerrainCluster& c = dag.clusters[i];
    if (proj(dag.ClusterOwnError(c), dag.ClusterOwnSphere(c)) > tau_px) {
      continue;  // this cluster is itself too coarse -> a finer one covers here
    }
    float parent_proj;
    if (c.parent_group == kNoGroup) {
      parent_proj = std::numeric_limits<float>::infinity();  // root: never drop
    } else {
      const TerrainClusterGroup& pg = dag.groups[c.parent_group];
      parent_proj = proj(pg.error_m, pg.sphere);
    }
    if (parent_proj > tau_px) out.push_back(i);
  }
}


TerrainClusterDag BuildTerrainClusterDag(const MapData& map,
                                         const TerrainClusterParams& params,
                                         const TerrainDetailField* detail) {
  const auto t_start = std::chrono::steady_clock::now();
  const float sp = map.spacing_m();
  const int Q = std::max(1, params.tile_quads);
  // Quads span the lattice: (nodes-1) quads over nodes vertices per axis.
  const int W = std::max(0, map.nodes_x() - 1);
  const int H = std::max(0, map.nodes_z() - 1);

  TerrainClusterDag dag;
  dag.map_quads_x = W;
  dag.map_quads_z = H;
  const float map_w = static_cast<float>(W) * sp;
  const float map_h = static_cast<float>(H) * sp;

  // Geometry of every cluster, parallel to dag.clusters, kept so each level can
  // consume the level below. EmitCluster appends to both in lock-step.
  std::vector<ClusterGeom> cluster_geom;

  // --- Level 0: grid-tile leaves -------------------------------------------
  const int budget = kTrisPerQuad * Q * Q;
  const DetailView dv = MakeDetailView(detail, W, H, budget);
  const int tiles_x = (W + Q - 1) / Q;
  const int tiles_z = (H + Q - 1) / Q;
  RegionGrid grid;
  grid.nx = tiles_x;
  grid.nz = tiles_z;
  grid.cells.resize(static_cast<size_t>(tiles_x) * tiles_z);
  for (int tz = 0; tz < tiles_z; ++tz) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      const int qx0 = tx * Q, qx1 = std::min((tx + 1) * Q, W);
      const int qz0 = tz * Q, qz1 = std::min((tz + 1) * Q, H);
      Region& r = grid.at(tx, tz);
      r.x0 = qx0 * sp;
      r.z0 = qz0 * sp;
      r.x1 = qx1 * sp;
      r.z1 = qz1 * sp;
      // A tile with NO refined quads takes exactly the pre-detail path, so a
      // null (or all-zero) detail field reproduces today's build bit for bit
      // -- and so does every cold tile of a detailed build, which is what
      // keeps the refinement provably local.
      bool hot = false;
      if (dv.field) {
        for (int qz = qz0; qz < qz1 && !hot; ++qz)
          for (int qx = qx0; qx < qx1 && !hot; ++qx)
            hot = dv.QuadExp(qx, qz) > 0;
      }
      if (!hot) {
        ClusterGeom leaf = BuildLeafGeom(map, qx0, qz0, qx1, qz1);
        const uint32_t cidx =
            EmitCluster(dag, cluster_geom, std::move(leaf), 0, kNoGroup);
        r.clusters = {cidx};
        continue;
      }
      // A hot tile emits one leaf cluster per cell of its sub-grid, then the
      // INTRA-REGION REDUCTION ROUNDS bring the region back to a single
      // cluster before the map-level merge ever sees it. The trigger is
      // purely structural -- more than one cluster in the region -- and the
      // reduction is ReduceGrid itself on the finer grid, not a second code
      // path. Emission stays serial and row-major throughout, so the DAG
      // layout is deterministic.
      TileCells cells =
          BuildDetailedTileCells(map, dv, qx0, qz0, qx1, qz1, Q);
      RegionGrid sub;
      sub.nx = cells.nx;
      sub.nz = cells.nz;
      sub.cells.resize(static_cast<size_t>(cells.nx) * cells.nz);
      for (int cz = 0; cz < cells.nz; ++cz) {
        for (int cx = 0; cx < cells.nx; ++cx) {
          ClusterGeom& gm = cells.geoms[static_cast<size_t>(cz) * cells.nx + cx];
          const uint32_t cidx =
              EmitCluster(dag, cluster_geom, std::move(gm), 0, kNoGroup);
          Region& sr = sub.at(cx, cz);
          const int ax0 = qx0 + cx * cells.side;
          const int az0 = qz0 + cz * cells.side;
          sr.x0 = ax0 * sp;
          sr.z0 = az0 * sp;
          sr.x1 = std::min(ax0 + cells.side, qx1) * sp;
          sr.z1 = std::min(az0 + cells.side, qz1) * sp;
          sr.clusters = {cidx};
        }
      }
      if (cells.nx * cells.nz > 1)
        ReduceGrid(dag, cluster_geom, sub, map_w, map_h, params,
                   /*prepass=*/true);
      r.clusters.clear();
      for (const Region& sr : sub.cells)
        for (uint32_t c : sr.clusters) r.clusters.push_back(c);
    }
  }

  ReduceGrid(dag, cluster_geom, grid, map_w, map_h, params);
  // 1 + the deepest level actually emitted -- NOT the loop count. Identical on
  // a uniform build (the last merge emits at cur); on a mixed-depth build the
  // loop count and the tree depth diverge, and every consumer sizing by
  // level_count (selection histogram, LogStats) needs the true depth.
  dag.level_count = 0;
  for (const TerrainCluster& c : dag.clusters)
    dag.level_count = std::max(dag.level_count, c.level + 1);

  const double build_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t_start)
          .count();
  LogStats(dag, build_ms, params.parallel_build, GetWorkerThreadCount());
  return dag;
}

}  // namespace badlands
