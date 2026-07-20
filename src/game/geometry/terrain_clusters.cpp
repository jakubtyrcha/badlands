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
#include "game/geometry/terrain_mesh.hpp"  // SampleHeight, NormalAt, PackU8x4
#include "mapgen/biomes.hpp"
#include "mapgen/mapgen_constants.hpp"

namespace badlands {

namespace {

using mapgen::Field2D;
using mapgen::kBiomePalette;
using mapgen::kMetersPerSample;

int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

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

// The 2-triangle-per-quad leaf tile [qx0,qx1] x [qz0,qz1] (in quads = samples),
// vertex grid at 1 sample spacing, one consistent diagonal (n00->n11).
ClusterGeom BuildLeafGeom(const Field2D<float>& height,
                          const Field2D<uint8_t>& biomes, int qx0, int qz0,
                          int qx1, int qz1) {
  const float mps = static_cast<float>(kMetersPerSample);
  const int vx = qx1 - qx0 + 1;
  const int vz = qz1 - qz0 + 1;
  ClusterGeom g;
  g.verts.reserve(static_cast<size_t>(vx) * vz);
  for (int j = qz0; j <= qz1; ++j) {
    for (int i = qx0; i <= qx1; ++i) {
      const float wx = static_cast<float>(i) * mps;
      const float wz = static_cast<float>(j) * mps;
      WorkVertex v;
      v.pos = glm::vec3(wx, SampleHeight(height, wx, wz), wz);
      v.normal = NormalAt(height, wx, wz);
      v.biome = biomes.at(clampi(i, 0, biomes.width - 1),
                          clampi(j, 0, biomes.height - 1));
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
                             const TerrainClusterParams& params) {
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
  const int budget = kTrisPerQuad * params.tile_quads * params.tile_quads;
  const int small_group_max =
      static_cast<int>(budget * kSmallGroupBudgetFactor);
  int num_out = (static_cast<int>(tri_count) <= small_group_max)
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

TerrainClusterDag BuildTerrainClusterDag(const Field2D<float>& heightmap,
                                         const Field2D<uint8_t>& biomes,
                                         const TerrainClusterParams& params) {
  const auto t_start = std::chrono::steady_clock::now();
  const float mps = static_cast<float>(kMetersPerSample);
  const int Q = std::max(1, params.tile_quads);
  const int W = heightmap.width;
  const int H = heightmap.height;

  TerrainClusterDag dag;
  dag.map_quads_x = W;
  dag.map_quads_z = H;
  const float map_w = static_cast<float>(W) * mps;
  const float map_h = static_cast<float>(H) * mps;

  // Geometry of every cluster, parallel to dag.clusters, kept so each level can
  // consume the level below. EmitCluster appends to both in lock-step.
  std::vector<ClusterGeom> cluster_geom;

  // --- Level 0: grid-tile leaves -------------------------------------------
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
      ClusterGeom leaf = BuildLeafGeom(heightmap, biomes, qx0, qz0, qx1, qz1);
      const uint32_t cidx =
          EmitCluster(dag, cluster_geom, std::move(leaf), 0, kNoGroup);
      Region& r = grid.at(tx, tz);
      r.x0 = qx0 * mps;
      r.z0 = qz0 * mps;
      r.x1 = qx1 * mps;
      r.z1 = qz1 * mps;
      r.clusters = {cidx};
    }
  }

  // --- Loop: group -> simplify -> split, until one root cluster remains -----
  int cur = 0;  // level of the clusters currently held in `grid`
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
    std::vector<GroupResult> results(group_count);
    auto compute = [&](size_t g) {
      ClusterGeom merged = WeldChildren(cluster_geom, work[g].children);
      results[g] = SimplifyAndSplit(std::move(merged), work[g].footprint, map_w,
                                    map_h, params);
    };
    if (params.parallel_build) {
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

        // Reserve the group slot, emit its outputs (own_group = this group).
        const int gidx = static_cast<int>(dag.groups.size());
        dag.groups.emplace_back();
        std::vector<uint32_t> out_clusters;
        for (ClusterGeom& out : gr.outputs)
          out_clusters.push_back(EmitCluster(dag, cluster_geom, std::move(out),
                                             cur + 1, gidx));

        // Group record: monotone error + a sphere nesting the children spheres
        // and the outputs' AABB.
        TerrainClusterGroup& G = dag.groups[gidx];
        G.level = cur + 1;
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
    // Free the just-consumed level's scratch geometry. Every level-`cur` cluster
    // was in `grid` and has now been welded into level cur+1; WeldChildren only
    // ever reads the live grid's level, so this geom is dead. cluster_geom is
    // parallel to dag.clusters and never serialized, so clearing it cannot
    // perturb the DAG output (the determinism/invariant tests stay green).
    for (uint32_t c = 0; c < cluster_geom.size(); ++c) {
      if (dag.clusters[c].level == cur) cluster_geom[c] = ClusterGeom{};
    }
    ++cur;
  }
  dag.level_count = cur + 1;

  const double build_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t_start)
          .count();
  LogStats(dag, build_ms, params.parallel_build, GetWorkerThreadCount());
  return dag;
}

}  // namespace badlands
