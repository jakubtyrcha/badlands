// Production-matching bench + GOLDEN OUTPUT PIN for the terrain cluster-LOD DAG
// build (terrain_clusters.cpp).
//
// Why this exists, and why it is separate from terrain_clusters_tests.cpp: the
// invariant suite runs on tiny synthetic heightfields with no detail field, and
// it pins PROPERTIES (monotonicity, seam equality, cut validity). It cannot
// catch an optimization that changes the output while keeping every property
// true. This file pins the OUTPUT ITSELF -- a hash of every byte of the DAG --
// on a fixture whose cost profile matches the production map.
//
// MATCHING PRODUCTION IS THE POINT. The measured 2048^2 build spends 80% of its
// time in the leaf pass and 76% of THAT in the 1.7% of tiles a river corridor
// refines, because RiverCarve::HeightAt is expensive (a bilinear base sample
// plus a per-texel candidate-arc scan) and every detail vertex costs five calls
// -- one for position, four for the central-difference normal. An analytic
// stand-in for the carve would be fast, and would therefore benchmark a build
// that does not exist. So this fixture drives the REAL mapgen carve through the
// same path map_view_view.cpp uses: river graph -> build_river_arcs ->
// build_river_carve -> dilated exponent grid -> TerrainDetailField.
//
// The fixture is otherwise fully analytic and seed-free: a grid of square
// pyramids on a tilted plane. Pyramid ridges give simplification real creases to
// respect (a smooth field collapses too easily to exercise the error metric),
// and the tilt gives the channel somewhere downhill to go.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "core/parallel.hpp"
#include "game/geometry/terrain_clusters.hpp"
#include "game/map/map_data.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/river_carve.hpp"
#include "mapgen/river_graph.hpp"

using badlands::BuildTerrainClusterDag;
using badlands::MapData;
using badlands::TerrainClusterDag;
using badlands::TerrainClusterParams;
using badlands::TerrainDetailField;

namespace {

// --- fixture constants -------------------------------------------------------
// Sized so the build is ~1/64 the production map (fast enough for ctest) while
// keeping the texel size and the channel geometry at production scale, which is
// what makes the per-hot-tile cost representative.
constexpr int kTexels = 256;             // heightmap is kTexels^2
constexpr float kWorldM = 256.0f;        // -> 1 m texels, as on the 2048 m map
constexpr float kPyramidPeriodM = 32.0f;  // pyramid grid pitch
constexpr float kPyramidAmpM = 12.0f;
constexpr float kTiltPerM = 0.06f;       // downstream slope of the base plane
constexpr float kTiltBaseM = 40.0f;

// Mirrors map_view_view.cpp: the arc fit tolerance is half a texel, and the
// corridor is refined at exponent 3 (0.125 m sampling at 1 m texels).
constexpr float kArcToleranceTexels = 0.5f;
constexpr uint8_t kDetailExponent = 3;

// Meander shape. Tuned so the corridor covers a fraction of the map comparable
// to production (~0.4% of texels, ~0.5% of quads, ~1.7% of TILES); the fixture
// asserts this, so a change to the carve cannot silently turn the fixture cold
// and make the benchmark measure nothing.
//
// Note what cannot be matched at this size: production carries 4645 m of channel
// on a 2048 m map, i.e. 2.3 map-widths. Reproducing that channel DENSITY at 256 m
// would put ~3% of texels in the corridor, because channel length scales
// linearly with the map while area scales quadratically. The fixture matches the
// FRACTION instead, because the fraction is what sets the cold/hot cost spread
// the leaf pass has to load-balance across.
constexpr float kChannelStartM = 24.0f;
constexpr float kChannelEndM = 104.0f;
constexpr float kMeanderAmpM = 12.0f;
constexpr float kMeanderWavelengthM = 60.0f;
constexpr float kPointStepM = 2.0f;
constexpr float kChannelWidthM = 1.2f;   // a small production headwater
constexpr float kChannelDepthM = 0.32f;

// --- base surface ------------------------------------------------------------

// Square pyramids on a kPyramidPeriodM grid, over a plane tilted down +X. Pure
// function of the texel index: no noise, no seed, no platform-dependent math
// beyond std::fmod/std::abs.
float BaseHeightAt(int x, int z) {
  const float fx = static_cast<float>(x);
  const float fz = static_cast<float>(z);
  const float u = std::abs(std::fmod(fx, kPyramidPeriodM) / kPyramidPeriodM - 0.5f);
  const float v = std::abs(std::fmod(fz, kPyramidPeriodM) / kPyramidPeriodM - 0.5f);
  // Chebyshev distance from the cell centre -> a square pyramid, apex at the
  // centre, meeting zero exactly on the cell boundary.
  const float pyramid = kPyramidAmpM * (1.0f - 2.0f * std::max(u, v));
  return kTiltBaseM - kTiltPerM * fx + pyramid;
}

badlands::mapgen::Field2D<float> MakeBaseHeight() {
  badlands::mapgen::Field2D<float> h(kTexels, kTexels, 0.0f);
  for (int j = 0; j < kTexels; ++j)
    for (int i = 0; i < kTexels; ++i) h.at(i, j) = BaseHeightAt(i, j);
  return h;
}

// The frozen MapData lattice, built exactly as MakeOneHotMapData does in
// map_view_view.cpp: ONE MORE NODE than texels per axis, edge nodes clamping to
// the last texel, so node i sits at i * texel_m and the lattice spans the map.
// Biome follows elevation, which is both deterministic and the way a real map
// correlates -- it gives the attribute metric something to weigh.
MapData MakeMapData(const badlands::mapgen::Field2D<float>& h) {
  const float texel_m = kWorldM / static_cast<float>(kTexels);
  MapData map(kTexels + 1, kTexels + 1, texel_m);
  for (int j = 0; j <= kTexels; ++j) {
    for (int i = 0; i <= kTexels; ++i) {
      const int sx = std::min(i, kTexels - 1), sz = std::min(j, kTexels - 1);
      const float height = h.at(sx, sz);
      map.mutable_height(i, j) = height;
      const badlands::mapgen::Biome b =
          height < 28.0f ? badlands::mapgen::Biome::Forest
                         : (height < 40.0f ? badlands::mapgen::Biome::Plains
                                           : badlands::mapgen::Biome::Hills);
      map.mutable_slice(static_cast<int>(b), i, j) = 255;
    }
  }
  return map;
}

// --- river -------------------------------------------------------------------

// A single reach meandering across the map. build_river_carve reads only
// `points_m` plus the per-point width/depth (and uses `g.nodes` for topology
// ordering across confluences, of which this fixture has none), so a hand-built
// graph drives exactly the production carve code.
badlands::mapgen::RiverGraph MakeMeanderGraph() {
  using badlands::mapgen::RiverEdge;
  using badlands::mapgen::RiverGraph;
  using badlands::mapgen::RiverNode;

  RiverGraph g;
  RiverEdge e;
  e.from = 0;
  e.to = 1;
  for (float x = kChannelStartM; x <= kChannelEndM; x += kPointStepM) {
    const float t = (x - kChannelStartM) / (kChannelEndM - kChannelStartM);
    const float z = 0.5f * kWorldM +
                    kMeanderAmpM * std::sin(6.28318530718f * x / kMeanderWavelengthM);
    e.points_m.push_back(glm::vec2(x, z));
    // Width and depth grow downstream, as hydraulic geometry gives (w ~ Q^0.5,
    // d ~ Q^0.3); the absolute scale is a small river, matching the corridor
    // widths the production map actually produces.
    const float grow = 0.6f + 0.4f * t;
    e.width_m.push_back(kChannelWidthM * grow);
    e.depth_m.push_back(kChannelDepthM * grow);
    e.speed_m_s.push_back(0.8f);
    e.discharge_m3_s.push_back(0.5f * grow);
  }
  g.edges.push_back(std::move(e));

  RiverNode src, mouth;
  src.pos_m = g.edges[0].points_m.front();
  mouth.pos_m = g.edges[0].points_m.back();
  g.nodes.push_back(src);
  g.nodes.push_back(mouth);
  return g;
}

// --- the whole fixture -------------------------------------------------------

struct Fixture {
  badlands::mapgen::Field2D<float> base;
  MapData map;
  // Heap-allocated and never moved: detail.height_at closes over a raw pointer
  // to it, exactly as map_view_view.cpp does, and a dangling pointer here would
  // be a silent wrong-height bug rather than a crash.
  std::unique_ptr<badlands::mapgen::RiverCarve> carve;
  std::vector<uint8_t> detail_level;
  TerrainDetailField detail;

  size_t corridor_texels = 0;
  size_t refined_quads = 0;
  size_t hot_tiles = 0, total_tiles = 0;
};

Fixture MakeFixture() {
  // The build logs a per-level table and a profile on EVERY call. Across a
  // hundred benchmark iterations that is both unreadable and a real share of the
  // measurement, so the bench runs silent; the [.][profile] case below turns
  // logging back on for a single build when the table is what you want.
  spdlog::set_level(spdlog::level::off);

  Fixture f;
  f.base = MakeBaseHeight();
  f.map = MakeMapData(f.base);

  const badlands::mapgen::RiverGraph graph = MakeMeanderGraph();
  const float texel_m = kWorldM / static_cast<float>(kTexels);
  const std::vector<badlands::mapgen::RiverArcChain> chains =
      badlands::mapgen::build_river_arcs(graph, kArcToleranceTexels * texel_m);
  f.carve = std::make_unique<badlands::mapgen::RiverCarve>(
      badlands::mapgen::build_river_carve(graph, chains, f.base, kWorldM));

  // The 2x2 DILATION is load-bearing and is copied from map_view_view.cpp on
  // purpose: LatticeNodeVertex gives a base-lattice node the COARSE height
  // unless ALL FOUR incident quads are refined, so marking only the quad a
  // corridor texel indexes leaves uncarved posts standing in the channel.
  const int mw = f.carve->mask.width, mh = f.carve->mask.height;
  f.detail_level.assign(static_cast<size_t>(mw) * mh, 0);
  for (int j = 0; j < mh; ++j) {
    for (int i = 0; i < mw; ++i) {
      if (f.carve->mask.at(i, j) == 0) continue;
      ++f.corridor_texels;
      for (int dz = -1; dz <= 0; ++dz) {
        for (int dx = -1; dx <= 0; ++dx) {
          const int qx = i + dx, qz = j + dz;
          if (qx >= 0 && qz >= 0 && qx < mw && qz < mh)
            f.detail_level[static_cast<size_t>(qz) * mw + qx] = kDetailExponent;
        }
      }
    }
  }
  for (uint8_t k : f.detail_level) f.refined_quads += (k > 0) ? 1 : 0;

  // Hot TILES, counted the way the build counts them: a tile is hot if any of
  // its kTileQuads^2 quads is refined. This is the fraction that matters -- a
  // hot tile costs ~350x a cold one, so it decides the load imbalance the leaf
  // pass has to schedule around.
  const int q = badlands::kTileQuads;
  const int tiles_x = (mw + q - 1) / q, tiles_z = (mh + q - 1) / q;
  f.total_tiles = static_cast<size_t>(tiles_x) * tiles_z;
  for (int tz = 0; tz < tiles_z; ++tz) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      bool hot = false;
      for (int j = tz * q; j < std::min((tz + 1) * q, mh) && !hot; ++j)
        for (int i = tx * q; i < std::min((tx + 1) * q, mw) && !hot; ++i)
          hot = f.detail_level[static_cast<size_t>(j) * mw + i] > 0;
      f.hot_tiles += hot ? 1 : 0;
    }
  }

  f.detail.level = f.detail_level.data();
  f.detail.width = mw;
  f.detail.height = mh;
  f.detail.height_at = [carve = f.carve.get()](float wx, float wz) {
    return carve->HeightAt(wx, wz);
  };
  return f;
}

// Built once and shared: the carve alone costs more than a DAG build at this
// size, and every case below wants the identical input.
const Fixture& TheFixture() {
  static const Fixture f = MakeFixture();
  return f;
}

// --- golden hash -------------------------------------------------------------

// FNV-1a over every byte of the DAG. Fields are hashed INDIVIDUALLY rather than
// by memcpy-ing the structs, so struct padding (whose contents are not
// guaranteed) can never move the hash.
struct Fnv {
  uint64_t h = 1469598103934665603ull;

  void Bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  template <class T>
  void Pod(const T& v) {
    Bytes(&v, sizeof(T));
  }
  template <class T>
  void Flat(const std::vector<T>& v) {
    Pod(static_cast<uint64_t>(v.size()));
    if (!v.empty()) Bytes(v.data(), v.size() * sizeof(T));
  }
};

uint64_t HashDag(const TerrainClusterDag& dag) {
  Fnv f;
  f.Flat(dag.vertices);
  f.Flat(dag.indices);
  f.Flat(dag.group_children);
  f.Pod(dag.map_quads_x);
  f.Pod(dag.map_quads_z);
  f.Pod(dag.level_count);
  f.Pod(static_cast<uint64_t>(dag.clusters.size()));
  for (const badlands::TerrainCluster& c : dag.clusters) {
    f.Pod(c.first_index);
    f.Pod(c.index_count);
    f.Pod(c.first_vertex);
    f.Pod(c.vertex_count);
    f.Pod(c.bounds.min);
    f.Pod(c.bounds.max);
    f.Pod(c.own_group);
    f.Pod(c.parent_group);
    f.Pod(c.level);
  }
  f.Pod(static_cast<uint64_t>(dag.groups.size()));
  for (const badlands::TerrainClusterGroup& g : dag.groups) {
    f.Pod(g.error_m);
    f.Pod(g.sphere);
    f.Pod(g.footprint);
    f.Pod(g.level);
    f.Pod(g.first_child);
    f.Pod(g.child_count);
  }
  return f.h;
}

}  // namespace

// The pinned output. Captured from the build code as it stood BEFORE the
// parallelization work, which is the only ordering that makes it evidence:
// every optimization in that series claims to leave the DAG bit-identical, and
// this constant is what turns the claim into a test.
//
// WHEN THIS FAILS: it means the DAG changed. If that was intentional (a genuine
// algorithm change, not an optimization), re-capture it deliberately and say so
// in the commit. Do NOT relax it to a tolerance -- a bit-identity claim either
// holds or is false.
//
// Caveat, honestly stated: this pins floating-point output for THIS toolchain.
// Nothing in CMakeLists.txt sets -ffast-math or -ffp-contract and meshoptimizer
// is deterministic, so it should be stable across builds here; a compiler or
// optimization-level change could still move it. The property tests in
// terrain_clusters_tests.cpp remain the portable guarantee.
constexpr uint64_t kGoldenDagHash = 0x4b4e13b7e370cfe3ull;

TEST_CASE("cluster bench fixture matches the production cost profile",
          "[terrain][cluster][golden]") {
  const Fixture& f = TheFixture();
  const double texel_frac =
      double(f.corridor_texels) / double(kTexels) / double(kTexels);
  const double quad_frac =
      double(f.refined_quads) / double(kTexels) / double(kTexels);
  const double tile_frac = double(f.hot_tiles) / double(f.total_tiles);
  WARN("corridor texels " << f.corridor_texels << " (" << texel_frac * 100.0
                          << "%), refined quads " << f.refined_quads << " ("
                          << quad_frac * 100.0 << "%), hot tiles " << f.hot_tiles
                          << "/" << f.total_tiles << " (" << tile_frac * 100.0
                          << "%)");

  // Production (2048^2, W7): 0.40% of texels in the corridor, 0.54% of quads
  // refined, 1.66% of tiles hot. Loose bounds -- the point is to catch a fixture
  // that has gone COLD (no refinement, so the benchmark stops measuring the hot
  // path) or one that has swollen until refinement is the whole map.
  CHECK(texel_frac > 0.001);
  CHECK(texel_frac < 0.015);
  CHECK(quad_frac > 0.001);
  CHECK(quad_frac < 0.015);
  CHECK(tile_frac > 0.005);
  CHECK(tile_frac < 0.06);
}

TEST_CASE("terrain cluster DAG output is bit-identical to the pinned golden",
          "[terrain][cluster][golden]") {
  const Fixture& f = TheFixture();
  const TerrainClusterDag dag =
      BuildTerrainClusterDag(f.map, TerrainClusterParams{}, &f.detail);

  REQUIRE(dag.level_count > 1);
  REQUIRE(!dag.clusters.empty());

  const uint64_t hash = HashDag(dag);
  WARN("golden DAG hash = 0x" << std::hex << hash << std::dec << "  ("
                              << dag.clusters.size() << " clusters, "
                              << dag.groups.size() << " groups, "
                              << dag.indices.size() / 3 << " tris)");
  CHECK(hash == kGoldenDagHash);
}

TEST_CASE("serial and parallel builds agree byte for byte",
          "[terrain][cluster][golden]") {
  const Fixture& f = TheFixture();

  TerrainClusterParams par;
  par.parallel_build = true;
  TerrainClusterParams ser;
  ser.parallel_build = false;

  const TerrainClusterDag a = BuildTerrainClusterDag(f.map, par, &f.detail);
  const TerrainClusterDag b = BuildTerrainClusterDag(f.map, ser, &f.detail);

  // parallel_build is documented as a SCHEDULING knob only. On the detailed
  // fixture it exercises the pre-pass path as well as the main merge, which the
  // invariant suite's detail-free maps never reach.
  CHECK(HashDag(a) == HashDag(b));
}

// One build with the per-level stats + Amdahl profile table printed. Hidden
// ([.]) so ctest and the benchmark run stay quiet; run it explicitly with
// `scripts/test.sh badlands_terrain_cluster_bench "[profile]"`.
TEST_CASE("terrain cluster DAG build profile", "[.][profile]") {
  const Fixture& f = TheFixture();
  spdlog::set_level(spdlog::level::info);
  const TerrainClusterDag dag =
      BuildTerrainClusterDag(f.map, TerrainClusterParams{}, &f.detail);
  spdlog::set_level(spdlog::level::off);
  CHECK(!dag.clusters.empty());
}

TEST_CASE("terrain cluster DAG build", "[.][bench]") {
  const Fixture& f = TheFixture();

  BENCHMARK("build with river detail (parallel)") {
    return BuildTerrainClusterDag(f.map, TerrainClusterParams{}, &f.detail)
        .clusters.size();
  };

  BENCHMARK("build with river detail (serial)") {
    TerrainClusterParams p;
    p.parallel_build = false;
    return BuildTerrainClusterDag(f.map, p, &f.detail).clusters.size();
  };

  BENCHMARK("build with no detail field (parallel)") {
    return BuildTerrainClusterDag(f.map, TerrainClusterParams{}, nullptr)
        .clusters.size();
  };
}
