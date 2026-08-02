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

// --- Build instrumentation ---------------------------------------------------
// Wall-clock accounting for the build, reported after LogStats. Its ONE job is
// to locate the serial fraction: the build is a level-synchronous fan-out with
// serial phases on both sides of every barrier, so the Amdahl floor -- not the
// parallel section -- is what decides whether more threads can help at all.
// Cost is one steady_clock pair per group / per tile, nanoseconds against a
// meshopt call, so it stays unconditionally on like LogStats.
//
// Phase B accumulates into a per-group slot (never a shared counter), so the
// numbers are race-free under ParallelFor and identical to a serial run.
using ProfClock = std::chrono::steady_clock;

double MsSince(ProfClock::time_point t) {
  return std::chrono::duration<double, std::milli>(ProfClock::now() - t).count();
}

// Per-group Phase B breakdown. weld = WeldChildren, simplify = the meshopt
// call, split = centroid sort + per-output compaction.
struct GroupTiming {
  double weld_ms = 0.0;
  double simplify_ms = 0.0;
  double split_ms = 0.0;

  double Total() const { return weld_ms + simplify_ms + split_ms; }
};

// One Phase B slot: its timing plus the triangle counts that show how much
// reduction the round actually bought.
struct GroupStat {
  GroupTiming t;
  uint64_t in_tris = 0, out_tris = 0;
};

// One reduction round (one level of one ReduceGrid). b_wall is elapsed time,
// b_cpu is summed per-group time -- b_cpu / (b_wall * workers) is the round's
// parallel efficiency, and b_cpu / b_wall its realized speedup.
struct RoundProfile {
  size_t groups = 0;
  uint64_t in_tris = 0, out_tris = 0;
  double a_ms = 0.0, b_wall_ms = 0.0, b_cpu_ms = 0.0;
  // Phase C splits into a SERIAL prefix that fixes the layout (c1) and a
  // PARALLEL pack (c2). Kept apart because only c1 counts against Amdahl.
  double c1_ms = 0.0, c2_ms = 0.0;
  GroupTiming parts;
  bool parallel = false;
};

// Whole-build accounting. Pre-pass rounds are SUMMED ACROSS TILES by round
// index (thousands of tiny grids, one row each) while the main merge keeps one
// row per round; both are serial-outer today, which is exactly what the report
// is meant to expose.
struct BuildProfile {
  double leaf_scan_ms = 0.0;   // the "is this tile hot" quad scan
  double leaf_cold_ms = 0.0;   // BuildLeafGeom on plain tiles
  double leaf_cells_ms = 0.0;  // BuildDetailedTileCells on refined tiles
  double leaf_emit_ms = 0.0;   // EmitCluster, both paths
  double prepass_ms = 0.0;     // per-tile ReduceGrid, wall
  // Wall clock of the leaf pass's two halves. The component timings above are
  // summed CPU across tiles, so leaf_fanout_ms vs their sum is the fan-out's
  // realized speedup; leaf_merge_ms is the serial arena concatenation.
  double leaf_fanout_ms = 0.0;
  // The arena merge splits the same way Phase C does: a SERIAL prefix over the
  // tiles' sizes (leaf_prefix_ms) then a PARALLEL copy (leaf_copy_ms).
  double leaf_prefix_ms = 0.0;
  double leaf_copy_ms = 0.0;
  double leaf_total_ms = 0.0;
  double main_ms = 0.0;
  size_t cold_tiles = 0, hot_tiles = 0, hot_cells = 0;
  std::vector<RoundProfile> prepass_rounds;
  std::vector<RoundProfile> main_rounds;

  // Fold a per-tile profile into this one. Tiles are profiled into their own
  // slots so the parallel leaf pass never shares a counter; this runs in the
  // serial merge afterwards. NOTE the units shift: with the tile pass parallel,
  // the leaf component timings are summed CPU time, not wall time.
  void MergeFrom(const BuildProfile& o) {
    leaf_scan_ms += o.leaf_scan_ms;
    leaf_cold_ms += o.leaf_cold_ms;
    leaf_cells_ms += o.leaf_cells_ms;
    leaf_emit_ms += o.leaf_emit_ms;
    prepass_ms += o.prepass_ms;
    cold_tiles += o.cold_tiles;
    hot_tiles += o.hot_tiles;
    hot_cells += o.hot_cells;
    for (size_t i = 0; i < o.prepass_rounds.size(); ++i)
      Add(prepass_rounds, i, o.prepass_rounds[i]);
  }

  // Accumulate `r` into row `round` of `rows`, growing as needed.
  static void Add(std::vector<RoundProfile>& rows, size_t round,
                  const RoundProfile& r) {
    if (rows.size() <= round) rows.resize(round + 1);
    RoundProfile& d = rows[round];
    d.groups += r.groups;
    d.in_tris += r.in_tris;
    d.out_tris += r.out_tris;
    d.a_ms += r.a_ms;
    d.b_wall_ms += r.b_wall_ms;
    d.b_cpu_ms += r.b_cpu_ms;
    d.c1_ms += r.c1_ms;
    d.c2_ms += r.c2_ms;
    d.parts.weld_ms += r.parts.weld_ms;
    d.parts.simplify_ms += r.parts.simplify_ms;
    d.parts.split_ms += r.parts.split_ms;
    d.parallel = d.parallel || r.parallel;
  }
};

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
    uint64_t h = static_cast<uint64_t>(k.x) * 73856093u;
    h ^= static_cast<uint64_t>(k.y) * 19349663u + (h << 6) + (h >> 2);
    h ^= static_cast<uint64_t>(k.z) * 83492791u + (h << 6) + (h >> 2);
    // FINALIZER, and it is load-bearing: PosTable masks the LOW bits, and the
    // low bits of the mix above are structurally dead for lattice geometry. A
    // grid-aligned coordinate i*spacing has ~16 trailing zero mantissa bits,
    // multiplying by an odd constant preserves trailing zeros exactly, and the
    // only term moving entropy downward is (h >> 2) -- not far enough.
    //
    // On a map whose height is bitwise constant (a flat plateau, a clamped sea
    // level, the blockout maps, MakeFlatMapData in the test suite) this is not
    // a slight imbalance: MEASURED, all 16641 keys of a 129x129 weld hashed to
    // ONE bucket, turning each weld into an O(n^2) probe walk.
    //
    // This was invisible while the welds used std::unordered_map, whose PRIME
    // bucket count folds the high bits back in for free. Power-of-two masking
    // does not, so the mixing has to be explicit here.
    h ^= h >> 32;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return static_cast<size_t>(h);
  }
};
PosKey KeyOf(const glm::vec3& p) {
  PosKey k;
  std::memcpy(&k.x, &p.x, 4);
  std::memcpy(&k.y, &p.y, 4);
  std::memcpy(&k.z, &p.z, 4);
  return k;
}

// Open-addressed position -> index table, replacing the
// std::unordered_map<PosKey, uint32_t, PosKeyHash> both welds used. That map is
// node-based: one allocation per distinct vertex, on a path that welds every
// vertex of every cluster at every level. Linear probing over a power-of-two
// table keeps the whole structure in two flat arrays and makes a probe three
// integer compares.
//
// The table only answers "have I seen this position"; the caller still appends
// vertices in encounter order, so first-seen index assignment -- and therefore
// the welded output -- is bit-identical to the map version.
class PosTable {
 public:
  static constexpr uint32_t kAbsent = 0xFFFFFFFFu;

  explicit PosTable(size_t expected_keys) { Rehash(CapacityFor(expected_keys)); }

  // Returns the value already stored for `k`; if `k` is new, stores
  // `value_if_new` and returns kAbsent to say so.
  uint32_t FindOrInsert(const PosKey& k, uint32_t value_if_new) {
    size_t i = PosKeyHash{}(k) & mask_;
    while (val_[i] != kAbsent) {
      if (key_[i] == k) return val_[i];
      i = (i + 1) & mask_;
    }
    key_[i] = k;
    val_[i] = value_if_new;
    // Keep the load factor at or under 1/2; linear probing degrades fast above
    // that. Sizing from the caller's bound normally makes this never fire.
    if (++size_ * 2 > mask_ + 1) Rehash((mask_ + 1) * 2);
    return kAbsent;
  }

 private:
  static size_t CapacityFor(size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2) cap <<= 1;
    return cap;
  }

  void Rehash(size_t cap) {
    std::vector<PosKey> old_key;
    std::vector<uint32_t> old_val;
    old_key.swap(key_);
    old_val.swap(val_);
    key_.assign(cap, PosKey{});
    val_.assign(cap, kAbsent);
    mask_ = cap - 1;
    for (size_t i = 0; i < old_val.size(); ++i) {
      if (old_val[i] == kAbsent) continue;
      size_t j = PosKeyHash{}(old_key[i]) & mask_;
      while (val_[j] != kAbsent) j = (j + 1) & mask_;
      key_[j] = old_key[i];
      val_[j] = old_val[i];
    }
  }

  std::vector<PosKey> key_;
  std::vector<uint32_t> val_;
  size_t mask_ = 0;
  size_t size_ = 0;
};

// Packs one cluster's geometry into PRE-SIZED buffer slices at the given
// offsets and returns its record. Writes only [first_vertex, +vertex_count) and
// [first_index, +index_count), so callers holding disjoint offsets may run this
// concurrently -- which is what lets emission be parallelized once a serial
// prefix pass has fixed the layout.
TerrainCluster PackClusterAt(TerrainClusterDag& dag, const ClusterGeom& geom,
                             int level, int own_group, uint32_t first_vertex,
                             uint32_t first_index) {
  TerrainCluster c;
  c.first_vertex = first_vertex;
  c.vertex_count = static_cast<uint32_t>(geom.verts.size());
  c.first_index = first_index;
  c.index_count = static_cast<uint32_t>(geom.tris.size());
  c.level = level;
  c.own_group = own_group;

  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  float* out = dag.vertices.data() +
               static_cast<size_t>(first_vertex) * kFloatsPerClusterVertex;
  for (const WorkVertex& v : geom.verts) {
    // Palette bytes are written RAW (no sRGB->linear decode), matching the
    // engine terrain_blend material: the G-buffer albedo target is linear
    // BGRA8Unorm and terrain_blend loads its textures as plain RGBA8Unorm (no
    // sRGB decode), so both treat 8-bit albedo as linear. Linearizing here would
    // make cluster terrain too dark. See the M4 color-path finding in the docs.
    const mapgen::Rgb col = kBiomePalette[v.biome];
    *out++ = v.pos.x;
    *out++ = v.pos.y;
    *out++ = v.pos.z;
    *out++ = v.normal.x;
    *out++ = v.normal.y;
    *out++ = v.normal.z;
    *out++ = PackU8x4(col.r, col.g, col.b, 255);
    *out++ = PackU8x4(v.biome, ClusterHashByte(v.pos),
                      static_cast<uint8_t>(level), 0);
    lo = glm::min(lo, v.pos);
    hi = glm::max(hi, v.pos);
  }
  for (size_t k = 0; k < geom.tris.size(); ++k)
    dag.indices[first_index + k] = first_vertex + geom.tris[k];
  c.bounds = Aabb::FromMinMax(lo, hi);
  return c;
}

// Appending form: grows the buffers, then packs at the end. Used where emission
// is naturally serial anyway (inside one tile's own arena).
uint32_t EmitCluster(TerrainClusterDag& dag,
                     std::vector<ClusterGeom>& cluster_geom, ClusterGeom geom,
                     int level, int own_group) {
  const uint32_t first_vertex = static_cast<uint32_t>(
      dag.vertices.size() / kFloatsPerClusterVertex);
  const uint32_t first_index = static_cast<uint32_t>(dag.indices.size());
  dag.vertices.resize(dag.vertices.size() +
                      geom.verts.size() * kFloatsPerClusterVertex);
  dag.indices.resize(dag.indices.size() + geom.tris.size());

  const TerrainCluster c =
      PackClusterAt(dag, geom, level, own_group, first_vertex, first_index);
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
  // QuadExp clamps silently per quad, so say it ONCE here: a producer that
  // asked for detail it did not get should hear about it rather than wonder
  // why the carve looks coarse.
  int asked = 0;
  for (int i = 0; i < W * H; ++i) asked = std::max(asked, int(detail->level[i]));
  if (asked > v.kmax)
    spdlog::warn(
        "TerrainDetailField: exponent {} exceeds what the {}-triangle cluster "
        "budget allows ({}); clamped",
        asked, budget, v.kmax);
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
void AppendQuadGeom(ClusterGeom& g, PosTable& weld, const MapData& map,
                    const DetailView& dv, int qx, int qz) {
  const float sp = map.spacing_m();
  auto add = [&](const WorkVertex& v) -> uint32_t {
    const uint32_t idx = static_cast<uint32_t>(g.verts.size());
    const uint32_t got = weld.FindOrInsert(KeyOf(v.pos), idx);
    if (got != PosTable::kAbsent) return got;
    g.verts.push_back(v);
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
      // A cell holds at most `side^2` quads, each contributing at most
      // (2^kmax + 1)^2 vertices before welding -- an upper bound, so the table
      // is allocated once and never rehashes.
      const size_t per_quad = static_cast<size_t>((1 << kmax) + 1) *
                              static_cast<size_t>((1 << kmax) + 1);
      PosTable weld(static_cast<size_t>(out.side) * out.side * per_quad);
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
  // Exact upper bounds: the weld can only ever shrink the vertex count, and
  // never touches the triangle count. Sizing the table from the bound means it
  // never rehashes.
  size_t max_verts = 0, total_tris = 0;
  for (uint32_t c : children) {
    max_verts += cluster_geom[c].verts.size();
    total_tris += cluster_geom[c].tris.size();
  }

  ClusterGeom merged;
  merged.verts.reserve(max_verts);
  merged.tris.reserve(total_tris);
  PosTable weld(max_verts);
  std::vector<uint32_t> remap;
  for (uint32_t cidx : children) {
    const ClusterGeom& cg = cluster_geom[cidx];
    remap.resize(cg.verts.size());
    for (size_t v = 0; v < cg.verts.size(); ++v) {
      const uint32_t nv = static_cast<uint32_t>(merged.verts.size());
      const uint32_t got = weld.FindOrInsert(KeyOf(cg.verts[v].pos), nv);
      if (got == PosTable::kAbsent) {
        merged.verts.push_back(cg.verts[v]);
        remap[v] = nv;
      } else {
        remap[v] = got;
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
                             bool collapse_to_one = false,
                             GroupTiming* timing = nullptr) {
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
  const auto t_simplify = ProfClock::now();
  const size_t got = meshopt_simplifyWithAttributes(
      simplified.data(), merged.tris.data(), index_count, positions.data(),
      vcount, sizeof(float) * 3, attrs.data(), sizeof(float) * 6, weights, 6,
      lock.data(), target, FLT_MAX, meshopt_SimplifyErrorAbsolute,
      &result_error);
  if (timing) timing->simplify_ms += MsSince(t_simplify);
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
  //
  // Centroids are computed ONCE into a flat array rather than inside the
  // comparator. The comparator form recomputed three random-access vertex loads
  // plus the vec3 average on EVERY comparison -- O(n log n) times, all of it
  // cache-missing -- and the split was measured at ~40% of all Phase B CPU,
  // nearly as much as meshoptimizer itself. The sort is over identical float
  // values in identical order, and std::stable_sort on equal keys yields the
  // same permutation, so the output is bit-identical (pinned by the golden hash).
  const auto t_split = ProfClock::now();
  const bool split_x = (footprint.z - footprint.x) >= (footprint.w - footprint.y);
  std::vector<float> centroid(tri_count);
  for (size_t t = 0; t < tri_count; ++t) {
    const glm::vec3& a = merged.verts[simplified[t * 3 + 0]].pos;
    const glm::vec3& b = merged.verts[simplified[t * 3 + 1]].pos;
    const glm::vec3& c = merged.verts[simplified[t * 3 + 2]].pos;
    const glm::vec3 m = (a + b + c) / 3.0f;
    centroid[t] = split_x ? m.x : m.z;
  }
  std::vector<uint32_t> order(tri_count);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(
      order.begin(), order.end(),
      [&](uint32_t l, uint32_t r) { return centroid[l] < centroid[r]; });

  // Welded index -> this output's local index. A STAMPED flat array rather than
  // a per-output hash map: the key is already a dense index into merged.verts,
  // so hashing it bought nothing and cost a node allocation per unique vertex.
  // The stamp makes the reset O(1) instead of O(vcount) per output. First-seen
  // ordering is preserved -- vertices are still appended in encounter order --
  // so the outputs are bit-identical.
  std::vector<uint32_t> local_of(vcount);
  std::vector<uint32_t> stamp(vcount, 0);

  res.outputs.resize(num_out);
  for (int o = 0; o < num_out; ++o) {
    const size_t lo = tri_count * o / num_out;
    const size_t hi = tri_count * (o + 1) / num_out;
    ClusterGeom& out = res.outputs[o];
    const uint32_t mark = static_cast<uint32_t>(o) + 1;
    for (size_t s = lo; s < hi; ++s) {
      const uint32_t t = order[s];
      for (int k = 0; k < 3; ++k) {
        const uint32_t wi = simplified[t * 3 + k];
        if (stamp[wi] != mark) {
          stamp[wi] = mark;
          local_of[wi] = static_cast<uint32_t>(out.verts.size());
          out.verts.push_back(merged.verts[wi]);
        }
        out.tris.push_back(local_of[wi]);
      }
    }
  }
  if (timing) timing->split_ms += MsSince(t_split);
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

// Where the build's wall time goes, and how much of it is STRUCTURALLY SERIAL
// today. The last two lines are the point: `spdup` per round is the realized
// speedup of the only parallel section (B_cpu / B_wall), and the Amdahl floor
// is what the build would still cost if that section became free. If the floor
// is most of the build, more threads inside Phase B cannot help and the
// partitioning has to change instead.
void LogProfile(const BuildProfile& p, double build_ms, unsigned workers) {
  auto table = [](const char* title, const std::vector<RoundProfile>& rows) {
    if (rows.empty()) return;
    spdlog::info("  {}", title);
    spdlog::info(
        "    rd  groups    in_tris   out_tris     A_ms    B_wall     B_cpu  "
        "spdup     C_ms   weld  simplfy   split");
    for (size_t i = 0; i < rows.size(); ++i) {
      const RoundProfile& r = rows[i];
      const double spd = r.b_wall_ms > 0.0 ? r.b_cpu_ms / r.b_wall_ms : 0.0;
      spdlog::info(
          "    {:<2}{}{:>7} {:>10} {:>10} {:>8.1f} {:>9.1f} {:>9.1f} {:>5.2f}x"
          " {:>8.1f} {:>6.1f} {:>8.1f} {:>7.1f}",
          i, r.parallel ? ' ' : '*', r.groups, r.in_tris, r.out_tris, r.a_ms,
          r.b_wall_ms, r.b_cpu_ms, spd, r.c1_ms + r.c2_ms, r.parts.weld_ms,
          r.parts.simplify_ms, r.parts.split_ms);
    }
  };

  spdlog::info("terrain cluster DAG build profile ({} workers, {:.1f} ms):",
               workers, build_ms);
  // Components are summed CPU across tiles; fan-out/merge are wall clock.
  const double leaf_cpu = p.leaf_scan_ms + p.leaf_cold_ms + p.leaf_cells_ms +
                          p.leaf_emit_ms + p.prepass_ms;
  spdlog::info(
      "  leaf pass {:.1f} ms = fanout {:.1f} wall ({:.1f} cpu, {:.2f}x) + "
      "prefix {:.1f} serial + copy {:.1f} parallel",
      p.leaf_total_ms, p.leaf_fanout_ms, leaf_cpu,
      p.leaf_fanout_ms > 0.0 ? leaf_cpu / p.leaf_fanout_ms : 0.0,
      p.leaf_prefix_ms, p.leaf_copy_ms);
  spdlog::info(
      "    cpu: scan {:.1f} | cold {:.1f} ({} tiles) | cells {:.1f} ({} tiles, "
      "{} cells) | emit {:.1f} | pre-pass {:.1f}",
      p.leaf_scan_ms, p.leaf_cold_ms, p.cold_tiles, p.leaf_cells_ms,
      p.hot_tiles, p.hot_cells, p.leaf_emit_ms, p.prepass_ms);
  spdlog::info("  main merge {:.1f} ms      (* = round ran serial)", p.main_ms);
  table("pre-pass rounds (summed across hot tiles):", p.prepass_rounds);
  table("main merge rounds:", p.main_rounds);

  double main_a = 0.0, main_b_wall = 0.0, main_b_cpu = 0.0, main_c1 = 0.0,
         main_c2 = 0.0;
  for (const RoundProfile& r : p.main_rounds) {
    main_a += r.a_ms;
    main_b_wall += r.b_wall_ms;
    main_b_cpu += r.b_cpu_ms;
    main_c1 += r.c1_ms;
    main_c2 += r.c2_ms;
  }
  // Serial = what no amount of threads can remove: the two layout-fixing prefix
  // passes plus Phase A. Everything else fans out.
  const double serial_ms = p.leaf_prefix_ms + main_a + main_c1;
  const double pct = build_ms > 0.0 ? 100.0 * serial_ms / build_ms : 0.0;
  spdlog::info(
      "  serial {:.1f} ms ({:.1f}%) = leaf prefix {:.1f} + phaseA {:.1f} + "
      "phaseC1 {:.1f}   |   parallel: leaf fanout {:.1f} + leaf copy {:.1f} + "
      "phaseC2 {:.1f} + phaseB {:.1f} wall / {:.1f} cpu ({:.2f}x)",
      serial_ms, pct, p.leaf_prefix_ms, main_a, main_c1, p.leaf_fanout_ms,
      p.leaf_copy_ms, main_c2, main_b_wall, main_b_cpu,
      main_b_wall > 0.0 ? main_b_cpu / main_b_wall : 0.0);
  spdlog::info(
      "  Amdahl: every parallel section free -> {:.1f} ms floor ({:.2f}x "
      "ceiling)",
      serial_ms, serial_ms > 0.0 ? build_ms / serial_ms : 0.0);
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
                const TerrainClusterParams& params, bool prepass = false,
                BuildProfile* prof = nullptr) {
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
    RoundProfile round;
    round.groups = group_count;
    const auto t_phase_a = ProfClock::now();

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
    round.a_ms = MsSince(t_phase_a);

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
    std::vector<GroupStat> stats(group_count);
    auto compute = [&](size_t g) {
      GroupStat& st = stats[g];
      const auto t_weld = ProfClock::now();
      ClusterGeom merged = WeldChildren(cluster_geom, work[g].children);
      st.t.weld_ms = MsSince(t_weld);
      st.in_tris = merged.tris.size() / 3;
      if (prepass &&
          static_cast<int>(merged.tris.size()) / 3 <= small_group_max) {
        // Pre-pass pass-through (see the function comment): the weld IS the
        // reduction, and an unsimplified output carries zero new error.
        results[g].result_error = 0.0f;
        results[g].outputs.clear();
        results[g].outputs.push_back(std::move(merged));
      } else {
        results[g] = SimplifyAndSplit(std::move(merged), work[g].footprint,
                                      map_w, map_h, params, terminal, &st.t);
      }
      for (const ClusterGeom& o : results[g].outputs)
        st.out_tris += o.tris.size() / 3;
    };
    // Tiny rounds (per-tile pre-passes shrink to a handful of groups) pay more
    // in ParallelFor synchronization than the work is worth; run them inline.
    const auto t_phase_b = ProfClock::now();
    round.parallel = params.parallel_build && group_count >= 16;
    if (round.parallel) {
      ParallelFor(group_count, compute);
    } else {
      for (size_t g = 0; g < group_count; ++g) compute(g);
    }
    round.b_wall_ms = MsSince(t_phase_b);
    for (const GroupStat& st : stats) {
      round.b_cpu_ms += st.t.Total();
      round.parts.weld_ms += st.t.weld_ms;
      round.parts.simplify_ms += st.t.simplify_ms;
      round.parts.split_ms += st.t.split_ms;
      round.in_tris += st.in_tris;
      round.out_tris += st.out_tris;
    }
    const auto t_phase_c = ProfClock::now();

    // Phase C (serial, deterministic): emit clusters/groups + build spheres in
    // the fixed g order. This is the only phase that mutates the DAG, so the
    // output layout is independent of how Phase B was scheduled.
    // C1 (serial): fix the LAYOUT with a prefix over g in the same order the
    // single loop used, so every id and buffer offset is what the serial build
    // would have assigned. O(groups), not O(geometry).
    //
    // out_level derives from the CHILDREN, not from the loop counter:
    // 1 + max(child.level). Identical on a uniform build (every child sits at
    // `cur`), but the loop counter stops being a tree property the moment a
    // region arrives pre-reduced at a deeper level -- and from then on `level`
    // is descriptive only (see the hpp).
    struct GroupEmit {
      uint32_t cluster = 0, vert = 0, index = 0, group_child = 0;
      int gidx = 0, out_level = 0;
    };
    std::vector<GroupEmit> emit(group_count);
    {
      GroupEmit at;
      at.cluster = static_cast<uint32_t>(dag.clusters.size());
      at.vert =
          static_cast<uint32_t>(dag.vertices.size() / kFloatsPerClusterVertex);
      at.index = static_cast<uint32_t>(dag.indices.size());
      at.group_child = static_cast<uint32_t>(dag.group_children.size());
      at.gidx = static_cast<int>(dag.groups.size());
      for (size_t g = 0; g < group_count; ++g) {
        emit[g] = at;
        for (uint32_t c : work[g].children)
          emit[g].out_level =
              std::max(emit[g].out_level, dag.clusters[c].level + 1);
        for (const ClusterGeom& o : results[g].outputs) {
          at.cluster += 1;
          at.vert += static_cast<uint32_t>(o.verts.size());
          at.index += static_cast<uint32_t>(o.tris.size());
        }
        at.group_child += static_cast<uint32_t>(work[g].children.size());
        at.gidx += 1;
      }
      dag.clusters.resize(at.cluster);
      dag.vertices.resize(static_cast<size_t>(at.vert) * kFloatsPerClusterVertex);
      dag.indices.resize(at.index);
      dag.group_children.resize(at.group_child);
      dag.groups.resize(static_cast<size_t>(at.gidx));
      cluster_geom.resize(at.cluster);
    }
    round.c1_ms = MsSince(t_phase_c);
    const auto t_phase_c2 = ProfClock::now();

    // C2 (parallel): pack geometry and build the group records. Every write goes
    // to this group's own reserved slice; every cross-group READ (a child's
    // level, error, sphere) targets an EARLIER round and is already final. The
    // one write outside the slice -- child.parent_group -- is still disjoint,
    // because Phase A built the children lists from a partition of the previous
    // grid, so no cluster is a child of two groups.
    auto emit_group = [&](size_t g) {
      const GroupEmit& e = emit[g];
      const std::vector<uint32_t>& children = work[g].children;
      const glm::vec4 footprint = work[g].footprint;
      GroupResult& gr = results[g];

      std::vector<uint32_t> out_clusters;
      uint32_t ci = e.cluster, v = e.vert, i = e.index;
      for (ClusterGeom& out : gr.outputs) {
        dag.clusters[ci] =
            PackClusterAt(dag, out, e.out_level, e.gidx, v, i);
        v += static_cast<uint32_t>(out.verts.size());
        i += static_cast<uint32_t>(out.tris.size());
        cluster_geom[ci] = std::move(out);
        out_clusters.push_back(ci);
        ++ci;
      }

      // Group record: monotone error + a sphere nesting the children spheres
      // and the outputs' AABB.
      TerrainClusterGroup& G = dag.groups[static_cast<size_t>(e.gidx)];
      G.level = e.out_level;
      G.footprint = footprint;
      G.first_child = e.group_child;
      G.child_count = static_cast<uint32_t>(children.size());
      float child_err = 0.0f;
      glm::vec3 center(0.0f);
      for (size_t k = 0; k < children.size(); ++k) {
        const uint32_t c = children[k];
        dag.group_children[e.group_child + k] = c;
        dag.clusters[c].parent_group = e.gidx;
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

      Region& nr = next.cells[g];
      nr.x0 = footprint.x;
      nr.z0 = footprint.y;
      nr.x1 = footprint.z;
      nr.z1 = footprint.w;
      nr.clusters = std::move(out_clusters);
    };
    if (round.parallel) {
      ParallelFor(group_count, emit_group);
    } else {
      for (size_t g = 0; g < group_count; ++g) emit_group(g);
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

    // c2 covers the parallel pack plus the scratch free that follows it.
    round.c2_ms = MsSince(t_phase_c2);
    if (prof)
      BuildProfile::Add(prepass ? prof->prepass_rounds : prof->main_rounds,
                        static_cast<size_t>(cur), round);
    ++cur;
  }
}

// --- the leaf pass, tile by tile ---------------------------------------------
//
// Tiles are INDEPENDENT and always were: BuildLeafGeom and BuildDetailedTileCells
// read only the const MapData and DetailView, and a hot tile's pre-pass reduction
// runs on its own sub-grid and never reads a neighbour's clusters. The single
// thing that coupled them was EmitCluster appending to one shared DAG -- which is
// an EMISSION ORDER dependency, not a data dependency.
//
// So each tile builds into a private arena and the arenas are concatenated
// afterwards in row-major tile order. Because that is exactly the order the
// serial loop visited tiles in, the running offsets applied during the merge
// equal the counts the serial build had accumulated at the same point: cluster
// ids, group ids and buffer offsets all come out identical, and the DAG is
// bit-identical to a serial build rather than merely equivalent.

struct TileBuild {
  TerrainClusterDag dag;              // arena-local; indices are arena-local
  std::vector<ClusterGeom> geom;      // parallel to dag.clusters
  std::vector<uint32_t> out_clusters;  // arena-local ids surviving in the region
  BuildProfile prof;
};

// Builds one tile into its own arena. `params` must have parallel_build OFF:
// with 65k tile tasks in flight the outer fan-out already saturates the pool, so
// an inner fan-out would only add synchronization -- and the measurement agrees
// (per-tile pre-pass rounds have a handful of groups and were already falling
// below the inline threshold). parallel_build is a scheduling knob only, so
// forcing it off here cannot change the output.
void BuildTile(const MapData& map, const DetailView& dv, int tx, int tz, int Q,
               int W, int H, float map_w, float map_h,
               const TerrainClusterParams& params, TileBuild& out) {
  const float sp = map.spacing_m();
  const int qx0 = tx * Q, qx1 = std::min((tx + 1) * Q, W);
  const int qz0 = tz * Q, qz1 = std::min((tz + 1) * Q, H);
  BuildProfile& prof = out.prof;

  // A tile with NO refined quads takes exactly the pre-detail path, so a null
  // (or all-zero) detail field reproduces today's build bit for bit -- and so
  // does every cold tile of a detailed build, which is what keeps the refinement
  // provably local.
  const auto t_scan = ProfClock::now();
  bool hot = false;
  if (dv.field) {
    for (int qz = qz0; qz < qz1 && !hot; ++qz)
      for (int qx = qx0; qx < qx1 && !hot; ++qx) hot = dv.QuadExp(qx, qz) > 0;
  }
  prof.leaf_scan_ms += MsSince(t_scan);

  if (!hot) {
    ++prof.cold_tiles;
    const auto t_cold = ProfClock::now();
    ClusterGeom leaf = BuildLeafGeom(map, qx0, qz0, qx1, qz1);
    prof.leaf_cold_ms += MsSince(t_cold);
    const auto t_emit = ProfClock::now();
    out.out_clusters = {
        EmitCluster(out.dag, out.geom, std::move(leaf), 0, kNoGroup)};
    prof.leaf_emit_ms += MsSince(t_emit);
    return;
  }

  ++prof.hot_tiles;
  // A hot tile emits one leaf cluster per cell of its sub-grid, then the
  // INTRA-REGION REDUCTION ROUNDS bring the region back to a single cluster
  // before the map-level merge ever sees it. The trigger is purely structural --
  // more than one cluster in the region -- and the reduction is ReduceGrid
  // itself on the finer grid, not a second code path.
  const auto t_cells = ProfClock::now();
  TileCells cells = BuildDetailedTileCells(map, dv, qx0, qz0, qx1, qz1, Q);
  prof.leaf_cells_ms += MsSince(t_cells);
  prof.hot_cells += static_cast<size_t>(cells.nx) * cells.nz;

  RegionGrid sub;
  sub.nx = cells.nx;
  sub.nz = cells.nz;
  sub.cells.resize(static_cast<size_t>(cells.nx) * cells.nz);
  for (int cz = 0; cz < cells.nz; ++cz) {
    for (int cx = 0; cx < cells.nx; ++cx) {
      ClusterGeom& gm = cells.geoms[static_cast<size_t>(cz) * cells.nx + cx];
      const auto t_emit = ProfClock::now();
      const uint32_t cidx =
          EmitCluster(out.dag, out.geom, std::move(gm), 0, kNoGroup);
      prof.leaf_emit_ms += MsSince(t_emit);
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
  if (cells.nx * cells.nz > 1) {
    const auto t_pre = ProfClock::now();
    ReduceGrid(out.dag, out.geom, sub, map_w, map_h, params, /*prepass=*/true,
               &prof);
    prof.prepass_ms += MsSince(t_pre);
  }
  for (const Region& sr : sub.cells)
    for (uint32_t c : sr.clusters) out.out_clusters.push_back(c);
}

// Where one tile's arena lands in the global DAG. A prefix sum over the tiles'
// sizes, so tile t's bases are exactly the totals the serial build had reached
// before visiting tile t.
struct TileOffset {
  uint32_t vert = 0;  // in VERTICES, not floats
  uint32_t index = 0;
  uint32_t cluster = 0;
  uint32_t group_child = 0;
  int group = 0;
};

// Copies a finished tile arena into its reserved slice of the global DAG,
// rebasing every index by that tile's offsets.
//
// Correctness has two halves. The OFFSETS come from a serial prefix over tiles
// in row-major order, so they equal the counts the serial build held at the same
// point -- ids and buffer positions are therefore identical, not merely
// consistent. The COPY then touches only tile t's own disjoint slice, so running
// the copies concurrently cannot change what any of them writes.
void AppendTileAt(TerrainClusterDag& dag, std::vector<ClusterGeom>& geom,
                  TileBuild& tile, Region& region, const TileOffset& o) {
  std::copy(tile.dag.vertices.begin(), tile.dag.vertices.end(),
            dag.vertices.begin() +
                static_cast<ptrdiff_t>(o.vert) * kFloatsPerClusterVertex);

  // Indices are stored ALREADY BIASED by their cluster's first_vertex (see
  // EmitCluster), so rebasing them is one add of the vertex offset.
  for (size_t k = 0; k < tile.dag.indices.size(); ++k)
    dag.indices[o.index + k] = tile.dag.indices[k] + o.vert;

  for (size_t k = 0; k < tile.dag.clusters.size(); ++k) {
    TerrainCluster c = tile.dag.clusters[k];
    c.first_vertex += o.vert;
    c.first_index += o.index;
    if (c.own_group != kNoGroup) c.own_group += o.group;
    if (c.parent_group != kNoGroup) c.parent_group += o.group;
    dag.clusters[o.cluster + k] = c;
  }
  for (size_t k = 0; k < tile.dag.groups.size(); ++k) {
    TerrainClusterGroup g = tile.dag.groups[k];
    g.first_child += o.group_child;
    dag.groups[static_cast<size_t>(o.group) + k] = g;
  }
  for (size_t k = 0; k < tile.dag.group_children.size(); ++k)
    dag.group_children[o.group_child + k] = tile.dag.group_children[k] + o.cluster;
  for (size_t k = 0; k < tile.geom.size(); ++k)
    geom[o.cluster + k] = std::move(tile.geom[k]);

  region.clusters.clear();
  for (uint32_t c : tile.out_clusters) region.clusters.push_back(c + o.cluster);

  // Release this arena as soon as it is copied. NOTE what this does and does
  // not buy: the global buffers are sized for the WHOLE leaf level before any
  // arena is freed, so THE PACKED LEAF LEVEL IS RESIDENT TWICE at peak. That
  // duplication is structural to build-into-arenas-then-concatenate, not
  // something this free avoids; what it avoids is holding the arenas for the
  // whole copy phase on top of it.
  //
  // MEASURED at 2048^2 (65536 tiles), at the moment of the merge:
  //   arena packed buffers   461.2 MB   <- duplicated; this is the cost
  //   arena ClusterGeom      239.0 MB   <- MOVED into cluster_geom, not copied
  //   dag packed buffers     461.2 MB
  // against a 3.40 GB peak RSS for the whole map load, so ~13%. Tolerable here,
  // but it scales with the map: 4096^2 would duplicate ~1.8 GB.
  //
  // The fix, when it matters, is to run the fan-out and merge in BATCHES of
  // tiles rather than all-then-all. Two things to know before trying it: the
  // global buffers would then grow incrementally instead of being sized once,
  // and the transient old+new during each reallocation eats part of the saving
  // (~1.5x rather than 1.0x) unless an exact reserve is computed up front --
  // which needs per-tile sizes, which needs the build, so it is circular. And
  // it puts a barrier per batch into the phase that currently scales best
  // (16.72x on 17 workers), which is the number to protect.
  tile.dag = TerrainClusterDag{};
  tile.geom.clear();
  tile.geom.shrink_to_fit();
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
  BuildProfile prof;
  const auto t_leaf = ProfClock::now();

  // Region rects first (pure arithmetic, and the merge below needs them).
  for (int tz = 0; tz < tiles_z; ++tz) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      const int qx0 = tx * Q, qx1 = std::min((tx + 1) * Q, W);
      const int qz0 = tz * Q, qz1 = std::min((tz + 1) * Q, H);
      Region& r = grid.at(tx, tz);
      r.x0 = qx0 * sp;
      r.z0 = qz0 * sp;
      r.x1 = qx1 * sp;
      r.z1 = qz1 * sp;
    }
  }

  // PHASE 1 (parallel): every tile into its own arena. Cost per tile spans ~350x
  // between a plain tile and a river-refined one, so this leans on ParallelFor's
  // per-item dynamic scheduling; any chunked split strands the expensive tiles.
  const size_t tile_count = grid.cells.size();
  std::vector<TileBuild> tiles(tile_count);
  TerrainClusterParams tile_params = params;
  tile_params.parallel_build = false;  // see BuildTile
  auto build_tile = [&](size_t t) {
    BuildTile(map, dv, static_cast<int>(t % tiles_x), static_cast<int>(t / tiles_x),
              Q, W, H, map_w, map_h, tile_params, tiles[t]);
  };
  const auto t_fanout = ProfClock::now();
  if (params.parallel_build && tile_count >= 16) {
    ParallelFor(tile_count, build_tile);
  } else {
    for (size_t t = 0; t < tile_count; ++t) build_tile(t);
  }
  prof.leaf_fanout_ms = MsSince(t_fanout);

  // PHASE 2a (serial, deterministic): prefix-sum the arena sizes in row-major
  // tile order. This is the step that fixes the layout -- and it is O(tiles),
  // not O(geometry), so it stays cheap however big the map gets.
  const auto t_merge = ProfClock::now();
  std::vector<TileOffset> offsets(tile_count);
  TileOffset running;
  for (size_t t = 0; t < tile_count; ++t) {
    prof.MergeFrom(tiles[t].prof);
    offsets[t] = running;
    const TerrainClusterDag& d = tiles[t].dag;
    running.vert += static_cast<uint32_t>(d.vertices.size() /
                                          kFloatsPerClusterVertex);
    running.index += static_cast<uint32_t>(d.indices.size());
    running.cluster += static_cast<uint32_t>(d.clusters.size());
    running.group_child += static_cast<uint32_t>(d.group_children.size());
    running.group += static_cast<int>(d.groups.size());
  }
  dag.vertices.resize(static_cast<size_t>(running.vert) * kFloatsPerClusterVertex);
  dag.indices.resize(running.index);
  dag.clusters.resize(running.cluster);
  dag.groups.resize(static_cast<size_t>(running.group));
  dag.group_children.resize(running.group_child);
  cluster_geom.resize(running.cluster);
  prof.leaf_prefix_ms = MsSince(t_merge);

  // PHASE 2b (parallel): each tile copies into its own reserved slice.
  const auto t_copy = ProfClock::now();
  auto merge_tile = [&](size_t t) {
    AppendTileAt(dag, cluster_geom, tiles[t], grid.cells[t], offsets[t]);
  };
  if (params.parallel_build && tile_count >= 16) {
    ParallelFor(tile_count, merge_tile);
  } else {
    for (size_t t = 0; t < tile_count; ++t) merge_tile(t);
  }
  prof.leaf_copy_ms = MsSince(t_copy);

  prof.leaf_total_ms = MsSince(t_leaf);

  const auto t_main = ProfClock::now();
  ReduceGrid(dag, cluster_geom, grid, map_w, map_h, params, /*prepass=*/false,
             &prof);
  prof.main_ms = MsSince(t_main);
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
  LogProfile(prof, build_ms, GetWorkerThreadCount());
  return dag;
}

}  // namespace badlands
