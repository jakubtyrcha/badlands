// Tests for the on-disk map form: the level convention, lake derivation, and
// the size checks that stand between a wrong manifest and a silently
// reinterpreted map (the rasters are headerless).

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/patch_io.hpp"
#include "mapgen/river_clip.hpp"
#include "mapgen/river_prune.hpp"

using namespace badlands::mapgen;

namespace {

// A scratch directory that cleans itself up, so a failing assertion cannot leak
// state into the next test.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / ("bl_map_io_" + name)) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

template <typename T>
void write_raw(const std::string& p, const std::vector<T>& v) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(v.data()),
          static_cast<std::streamsize>(v.size() * sizeof(T)));
}

void write_manifest(const std::string& dir, int res, float size_m) {
  std::ofstream f(dir + "/map.txt");
  f << "resolution " << res << "\nworld_size_m " << size_m
    << "\nsource unit-test\n";
}

// Writes a complete, valid map dir. `level` follows the convention: dry texels
// store level == height.
void write_map(const std::string& dir, int n, float size_m,
               const std::vector<float>& height, const std::vector<float>& level,
               const std::vector<uint8_t>& biome) {
  write_manifest(dir, n, size_m);
  write_raw(dir + "/height.f32", height);
  write_raw(dir + "/level.f32", level);
  write_raw(dir + "/biome.u8", biome);
}

}  // namespace

TEST_CASE("dry texels store level == height and give exactly zero depth",
          "[map_io]") {
  const int n = 8;
  Field2D<float> h(n, n, 100.0f), level(n, n, 100.0f);
  // One 2x2 pond at surface 105.
  for (int y = 2; y < 4; ++y)
    for (int x = 2; x < 4; ++x) {
      h.at(x, y) = 101.0f;
      level.at(x, y) = 105.0f;
    }
  Field2D<float> depth;
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  derive_water(h, level, 10.0f, depth, lake_id, lakes);

  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const bool wet = (x >= 2 && x < 4 && y >= 2 && y < 4);
      // Exactly zero, not merely small: a dry texel that reports a hair of
      // depth would render a film of water over the whole map.
      CHECK(depth.at(x, y) == (wet ? 4.0f : 0.0f));
      CHECK((lake_id.at(x, y) >= 0) == wet);
    }
  REQUIRE(lakes.size() == 1);
  CHECK(lakes[0].level_m == 105.0f);
  CHECK(lakes[0].max_depth_m == 4.0f);
  CHECK(lakes[0].area_m2 == Catch::Approx(4 * 100.0f));  // 4 texels of 10x10 m
}

TEST_CASE("a level below the bed never produces negative depth", "[map_io]") {
  const int n = 4;
  Field2D<float> h(n, n, 50.0f), level(n, n, 10.0f);  // level well under the bed
  Field2D<float> depth;
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  derive_water(h, level, 1.0f, depth, lake_id, lakes);
  for (float d : depth.data) CHECK(d == 0.0f);
  CHECK(lakes.empty());
}

TEST_CASE("two basins at different levels stay separate lakes", "[map_io]") {
  const int n = 16;
  Field2D<float> h(n, n, 200.0f), level(n, n, 200.0f);
  // Left pond at 105, right pond at 130 -- separated by high dry ground, so a
  // level test alone would not tell them apart if connectivity were ignored.
  for (int y = 2; y < 5; ++y)
    for (int x = 2; x < 5; ++x) {
      h.at(x, y) = 100.0f;
      level.at(x, y) = 105.0f;
    }
  for (int y = 10; y < 13; ++y)
    for (int x = 10; x < 13; ++x) {
      h.at(x, y) = 120.0f;
      level.at(x, y) = 130.0f;
    }
  Field2D<float> depth;
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  derive_water(h, level, 4.0f, depth, lake_id, lakes);

  REQUIRE(lakes.size() == 2);
  const int32_t left = lake_id.at(3, 3), right = lake_id.at(11, 11);
  CHECK(left != right);
  CHECK(lakes[left].level_m == 105.0f);
  CHECK(lakes[right].level_m == 130.0f);
  CHECK(lakes[left].max_depth_m == 5.0f);
  CHECK(lakes[right].max_depth_m == 10.0f);
}

TEST_CASE("a lake surface loads exactly flat", "[map_io]") {
  // The level raster is the per-lake constant, so bed detail under the water
  // must not tilt the surface. Reconstructing height + depth has to give one
  // value to the bit.
  const int n = 12;
  Field2D<float> h(n, n, 300.0f), level(n, n, 300.0f);
  for (int y = 3; y < 9; ++y)
    for (int x = 3; x < 9; ++x) {
      h.at(x, y) = 250.0f + 3.0f * ((x * 7 + y * 13) % 11);  // ragged bed
      level.at(x, y) = 290.0f;
    }
  Field2D<float> depth;
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  derive_water(h, level, 2.0f, depth, lake_id, lakes);
  REQUIRE(lakes.size() == 1);
  for (int y = 3; y < 9; ++y)
    for (int x = 3; x < 9; ++x)
      CHECK(h.at(x, y) + depth.at(x, y) == 290.0f);
}

TEST_CASE("load_patch round-trips a written directory", "[map_io]") {
  TempDir dir("roundtrip");
  const int n = 8;
  const float size_m = 64.0f;
  std::vector<float> height(n * n), level(n * n);
  std::vector<uint8_t> biome(n * n, uint8_t(Biome::Plains));
  for (int i = 0; i < n * n; ++i) {
    height[i] = 100.0f + i;
    level[i] = height[i];  // all dry
  }
  height[20] = 50.0f;
  level[20] = 60.0f;  // one wet texel, 10 m deep
  biome[20] = uint8_t(Biome::Lake);
  write_map(dir.str(), n, size_m, height, level, biome);

  std::string err;
  const auto p = load_patch(dir.str(), &err);
  REQUIRE(p.has_value());
  CHECK(err.empty());
  CHECK(p->height.width == n);
  CHECK(p->height.height == n);
  CHECK(p->height.data == height);
  CHECK(p->biome.data == biome);
  REQUIRE(p->lakes.size() == 1);
  CHECK(p->lakes[0].max_depth_m == 10.0f);
  CHECK(p->water_depth.data[20] == 10.0f);
  CHECK(p->lake_id.data[20] == 0);
  CHECK(p->lake_id.data[0] == -1);
}

TEST_CASE("a raster whose size contradicts the manifest is rejected",
          "[map_io]") {
  // The rasters carry no header, so this check is the only thing between a
  // wrong resolution and a map silently reinterpreted at the wrong stride.
  TempDir dir("badsize");
  const int n = 8;
  std::vector<float> height(n * n, 1.0f), level(n * n, 1.0f);
  std::vector<uint8_t> biome(n * n, uint8_t(Biome::Plains));
  write_map(dir.str(), n, 64.0f, height, level, biome);
  write_manifest(dir.str(), 16, 64.0f);  // claims 16x16, files hold 8x8

  std::string err;
  const auto art = load_patch(dir.str(), &err);
  CHECK_FALSE(art.has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("expected"));
}

TEST_CASE("an out-of-range biome is rejected", "[map_io]") {
  // A bad index silently samples the wrong terrain texture array layer, so it
  // must fail here rather than on the GPU.
  TempDir dir("badbiome");
  const int n = 4;
  std::vector<float> height(n * n, 1.0f), level(n * n, 1.0f);
  std::vector<uint8_t> biome(n * n, uint8_t(Biome::Plains));
  biome[5] = 200;
  write_map(dir.str(), n, 16.0f, height, level, biome);

  std::string err;
  CHECK_FALSE(load_patch(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("biome.u8"));
}

TEST_CASE("a missing or malformed manifest is reported, not guessed",
          "[map_io]") {
  TempDir dir("nomanifest");
  std::string err;
  CHECK_FALSE(load_patch_manifest(dir.str(), &err).has_value());
  CHECK_FALSE(err.empty());

  std::ofstream(dir.str() + "/map.txt") << "world_size_m 64\n";  // no resolution
  err.clear();
  CHECK_FALSE(load_patch_manifest(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("resolution"));
}

TEST_CASE("unknown manifest keys are ignored so the writer can add fields",
          "[map_io]") {
  TempDir dir("extrakeys");
  std::ofstream(dir.str() + "/map.txt")
      << "# a comment\nresolution 4\nworld_size_m 32\nsource protogen tag=x\n"
         "future_key 1234\n";
  std::string err;
  const auto man = load_patch_manifest(dir.str(), &err);
  REQUIRE(man.has_value());
  CHECK(man->resolution == 4);
  CHECK(man->world_size_m == 32.0f);
  CHECK(man->source == "protogen tag=x");
}

TEST_CASE("write_patch + load_patch round-trips a PatchData bit-identically",
          "[map_io]") {
  // Unlike the raster round-trip above, this goes through write_patch itself
  // -- the path a real provider uses -- rather than hand-written rasters.
  TempDir dir("write_roundtrip");
  const int n = 6;
  const float size_m = 48.0f;

  PatchData p;
  p.texel_m = size_m / static_cast<float>(n);
  p.origin_m = glm::dvec2(100.0, -50.0);
  p.height = Field2D<float>(n, n);
  p.level = Field2D<float>(n, n);
  p.biome = Field2D<uint8_t>(n, n, uint8_t(Biome::Plains));
  p.soil = Field2D<float>(n, n);
  for (int i = 0; i < n * n; ++i) {
    p.height.data[i] = 100.0f + 0.5f * static_cast<float>(i);
    p.level.data[i] = p.height.data[i];  // dry by default
    p.soil.data[i] = 0.1f * static_cast<float>(i);
  }
  p.level.data[7] = p.height.data[7] + 3.0f;  // one wet texel, 3 m deep
  p.biome.data[7] = uint8_t(Biome::Lake);
  derive_water(p.height, p.level, p.texel_m, p.water_depth, p.lake_id, p.lakes);

  std::string err;
  REQUIRE(write_patch(dir.str(), p, "unit-test", &err));
  CHECK(err.empty());

  const auto loaded = load_patch(dir.str(), &err);
  REQUIRE(loaded.has_value());
  CHECK(err.empty());
  CHECK(loaded->height.data == p.height.data);
  CHECK(loaded->level.data == p.level.data);
  CHECK(loaded->biome.data == p.biome.data);
  CHECK(loaded->soil.data == p.soil.data);
  // The derived water block is not written -- load_patch reproduces it from
  // height + level, so it must match what the same derivation gave `p`.
  CHECK(loaded->water_depth.data == p.water_depth.data);
  CHECK(loaded->lake_id.data == p.lake_id.data);
  REQUIRE(loaded->lakes.size() == p.lakes.size());
  for (size_t i = 0; i < p.lakes.size(); ++i) {
    CHECK(loaded->lakes[i].level_m == p.lakes[i].level_m);
    CHECK(loaded->lakes[i].max_depth_m == p.lakes[i].max_depth_m);
  }
  // `rivers` is not serialized yet -- deliberately not asserted on here.
}

TEST_CASE("soil absent on disk loads as zeros, correctly sized",
          "[map_io]") {
  // SOIL IS REQUIRED BY THE CONTRACT BUT OPTIONAL ON DISK (patch_io.hpp): a
  // directory written before the two-layer substrate has no soil.f32 at all,
  // and must still load with PatchData::soil present and zeroed rather than
  // failing or leaving it empty.
  TempDir dir("nosoil");
  const int n = 5;
  std::vector<float> height(n * n, 10.0f), level(n * n, 10.0f);
  std::vector<uint8_t> biome(n * n, uint8_t(Biome::Plains));
  write_map(dir.str(), n, 40.0f, height, level, biome);  // no soil.f32 written

  std::string err;
  const auto p = load_patch(dir.str(), &err);
  REQUIRE(p.has_value());
  CHECK(err.empty());
  CHECK(p->soil.width == n);
  CHECK(p->soil.height == n);
  for (float s : p->soil.data) CHECK(s == 0.0f);
}

// --- window_rivers graph surgery ------------------------------------------
//
// These cover clip/prune invariants that were silently violated: they produce a
// plausible-looking graph, so nothing fails until a trunk river vanishes.

TEST_CASE("clipping preserves geometry-less through-lake edges", "[window_rivers]") {
  // extract_river_graph emits inlet -> outlet edges with NO points on purpose,
  // so the network stays traversable river -> lake -> river. Dropping them for
  // having < 2 points severed that link AND left the lake's outflow reach with
  // in_deg == 0, so the length prune saw a headwater and could delete the trunk.
  RiverGraph g;
  g.nodes.resize(3);
  g.nodes[0].kind = RiverNodeKind::LakeInlet;
  g.nodes[1].kind = RiverNodeKind::LakeOutlet;
  g.nodes[2].kind = RiverNodeKind::Mouth;
  RiverEdge through;  // no geometry at all
  through.from = 0;
  through.to = 1;
  RiverEdge below;    // the outflow reach, well inside the window
  below.from = 1;
  below.to = 2;
  below.points_m = {{10.0f, 10.0f}, {40.0f, 40.0f}};
  below.discharge_m3_s = {1.0f, 1.0f};
  below.width_m = {2.0f, 2.0f};
  below.depth_m = {0.5f, 0.5f};
  below.speed_m_s = {1.0f, 1.0f};
  g.edges = {through, below};

  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));
  CHECK(g.edges.size() == 2);
  prune_river_graph_by_width(g, 0.5f);
  CHECK(g.edges.size() == 2);
  // With the connector intact the outflow reach is not a headwater, so a length
  // prune cannot mistake it for a stub.
  prune_river_graph_by_length(g, 1000.0f);
  bool outflow_survives = false;
  for (const RiverEdge& e : g.edges)
    if (e.points_m.size() == 2) outflow_survives = true;
  CHECK(outflow_survives);
}

TEST_CASE("every clipped reach is anchored at both ends", "[window_rivers]") {
  // A run whose last inside sample sat exactly on the frame exited without ever
  // assigning `to`, leaving a downstream endpoint no node explained.
  RiverGraph g;
  g.nodes.resize(2);
  RiverEdge e;
  e.from = 0;
  e.to = 1;
  // Crosses the +x frame exactly on a sample, then continues into the ghost.
  e.points_m = {{50.0f, 50.0f}, {80.0f, 50.0f}, {100.0f, 50.0f}, {130.0f, 50.0f}};
  e.discharge_m3_s = {1.0f, 1.0f, 1.0f, 1.0f};
  e.width_m = {2.0f, 2.0f, 2.0f, 2.0f};
  e.depth_m = {0.5f, 0.5f, 0.5f, 0.5f};
  e.speed_m_s = {1.0f, 1.0f, 1.0f, 1.0f};
  g.edges = {e};

  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));
  REQUIRE(!g.edges.empty());
  for (const RiverEdge& r : g.edges) {
    CHECK(r.from >= 0);
    CHECK(r.to >= 0);
    CHECK(r.to < static_cast<int32_t>(g.nodes.size()));
    // and nothing survives beyond the frame
    for (const glm::vec2& p : r.points_m) CHECK(p.x <= 100.0f);
  }
}

TEST_CASE("a reach entering the window gets a frame node too", "[window_rivers]") {
  // The exit crossing was minted but the entry was not, so a reach began at
  // whichever resampled sample first landed inside -- the very failure clipping
  // exists to prevent, mirrored upstream.
  RiverGraph g;
  g.nodes.resize(2);
  RiverEdge e;
  e.from = 0;
  e.to = 1;
  e.points_m = {{-30.0f, 50.0f}, {-10.0f, 50.0f}, {20.0f, 50.0f}, {60.0f, 50.0f}};
  e.discharge_m3_s = {1.0f, 1.0f, 1.0f, 1.0f};
  e.width_m = {2.0f, 2.0f, 2.0f, 2.0f};
  e.depth_m = {0.5f, 0.5f, 0.5f, 0.5f};
  e.speed_m_s = {1.0f, 1.0f, 1.0f, 1.0f};
  g.edges = {e};

  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));
  REQUIRE(g.edges.size() == 1);
  // The first point is ON the frame (x == 0), not at the first inside sample.
  CHECK(g.edges[0].points_m.front().x == Catch::Approx(0.0f).margin(1e-3));
  CHECK(g.edges[0].from >= 0);
}

TEST_CASE("length pruning never severs the network", "[window_rivers]") {
  // Only headwater chains may be dropped; an interior reach is a real river,
  // just a short one, and removing it would strand everything above.
  RiverGraph g;
  g.nodes.resize(4);
  const auto mk = [](int32_t f, int32_t t, float x0, float x1) {
    RiverEdge e;
    e.from = f;
    e.to = t;
    e.points_m = {{x0, 50.0f}, {x1, 50.0f}};
    e.discharge_m3_s = {1.0f, 1.0f};
    e.width_m = {2.0f, 2.0f};
    e.depth_m = {0.5f, 0.5f};
    e.speed_m_s = {1.0f, 1.0f};
    return e;
  };
  g.edges = {mk(0, 1, 10.0f, 60.0f),   // long headwater
             mk(2, 1, 30.0f, 32.0f),   // 2 m stub headwater -> should go
             mk(1, 3, 60.0f, 61.0f)};  // 1 m INTERIOR reach -> must stay
  prune_river_graph_by_length(g, 32.0f);
  bool interior = false, stub = false;
  for (const RiverEdge& e : g.edges) {
    const float len = std::abs(e.points_m.back().x - e.points_m.front().x);
    if (len == Catch::Approx(1.0f)) interior = true;
    if (len == Catch::Approx(2.0f)) stub = true;
  }
  CHECK(interior);
  CHECK_FALSE(stub);
}
