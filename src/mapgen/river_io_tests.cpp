// Tests for rivers.bin -- the binary RiverGraph dump written alongside
// world.txt. Text would be ~35 MB at 16 km, so this exercises the little-
// endian POD form directly: exact round trip, and rejection of a truncated
// file rather than a silent misread.
//
// HARD RULE: no test here may run the coarse sim. Every RiverGraph is
// hand-built.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "mapgen/river_io.hpp"

using namespace badlands::mapgen;

namespace {

// Same TempDir pattern as patch_io_tests.cpp, its own prefix so a parallel
// ctest run cannot collide with coarse_io_tests.cpp's.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / ("bl_river_io_" + name)) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

// A small graph exercising every node kind (including FrameEntry, the one
// most likely to be forgotten since it was appended last to the enum) and
// both a geometry-bearing reach and a through-lake connector (points_m
// empty, per river_network.hpp's contract for a lake-crossing edge).
RiverGraph make_graph() {
  RiverGraph g;
  g.nodes.resize(6);

  g.nodes[0].kind = RiverNodeKind::Source;
  g.nodes[0].pos_m = glm::vec2(10.0f, 20.0f);
  g.nodes[0].ground_m = 150.5f;
  g.nodes[0].drainage_area_m2 = 256.0f;
  g.nodes[0].discharge_m3_s = 0.01f;
  g.nodes[0].width_m = 1.0f;
  g.nodes[0].depth_m = 0.1f;
  g.nodes[0].speed_m_s = 0.5f;
  g.nodes[0].lake_id = -1;

  g.nodes[1].kind = RiverNodeKind::Confluence;
  g.nodes[1].pos_m = glm::vec2(30.0f, 40.0f);
  g.nodes[1].ground_m = 140.0f;

  g.nodes[2].kind = RiverNodeKind::LakeInlet;
  g.nodes[2].pos_m = glm::vec2(50.0f, 60.0f);
  g.nodes[2].lake_id = 3;
  g.nodes[2].lake_kind = LakeKind::Seeded;

  g.nodes[3].kind = RiverNodeKind::LakeOutlet;
  g.nodes[3].pos_m = glm::vec2(70.0f, 60.0f);
  g.nodes[3].lake_id = 3;
  g.nodes[3].lake_kind = LakeKind::Seeded;

  g.nodes[4].kind = RiverNodeKind::Mouth;
  g.nodes[4].pos_m = glm::vec2(100.0f, 500.0f);

  g.nodes[5].kind = RiverNodeKind::FrameEntry;
  g.nodes[5].pos_m = glm::vec2(-5.0f, 500.0f);

  RiverEdge e0;
  e0.from = 0;
  e0.to = 1;
  e0.points_m = {{10.0f, 20.0f}, {15.0f, 25.0f}, {20.0f, 30.0f}};
  e0.discharge_m3_s = {0.01f, 0.02f, 0.03f};
  e0.width_m = {1.0f, 1.2f, 1.4f};
  e0.depth_m = {0.1f, 0.12f, 0.14f};
  e0.speed_m_s = {0.5f, 0.55f, 0.6f};
  e0.strahler_order = 1;
  e0.shreve_magnitude = 1;

  RiverEdge e1;  // through-lake connector: no geometry at all
  e1.from = 2;
  e1.to = 3;
  e1.strahler_order = 2;
  e1.shreve_magnitude = 3;

  RiverEdge e2;
  e2.from = 5;
  e2.to = 4;
  e2.points_m = {{-5.0f, 500.0f}, {0.0f, 500.0f}};
  e2.discharge_m3_s = {5.0f, 5.25f};
  e2.width_m = {10.0f, 10.5f};
  e2.depth_m = {2.0f, 2.1f};
  e2.speed_m_s = {1.5f, 1.6f};
  e2.strahler_order = 4;
  e2.shreve_magnitude = 7;

  g.edges = {e0, e1, e2};
  return g;
}

}  // namespace

TEST_CASE("river graph round-trips exactly", "[artifact]") {
  TempDir dir("roundtrip");
  const RiverGraph g = make_graph();
  const std::string path = dir.str() + "/rivers.bin";

  std::string err;
  REQUIRE(write_river_graph(path, g, &err));
  CHECK(err.empty());

  const auto loaded = read_river_graph(path, &err);
  REQUIRE(loaded.has_value());
  CHECK(err.empty());

  REQUIRE(loaded->nodes.size() == g.nodes.size());
  REQUIRE(loaded->edges.size() == g.edges.size());

  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const RiverNode& a = g.nodes[i];
    const RiverNode& b = loaded->nodes[i];
    CHECK(a.pos_m == b.pos_m);
    CHECK(a.ground_m == b.ground_m);
    CHECK(a.drainage_area_m2 == b.drainage_area_m2);
    CHECK(a.discharge_m3_s == b.discharge_m3_s);
    CHECK(a.width_m == b.width_m);
    CHECK(a.depth_m == b.depth_m);
    CHECK(a.speed_m_s == b.speed_m_s);
    CHECK(a.lake_id == b.lake_id);
    CHECK(a.lake_kind == b.lake_kind);
    CHECK(a.kind == b.kind);  // every kind, including FrameEntry (node 5)
  }

  for (size_t i = 0; i < g.edges.size(); ++i) {
    const RiverEdge& a = g.edges[i];
    const RiverEdge& b = loaded->edges[i];
    CHECK(a.from == b.from);
    CHECK(a.to == b.to);
    CHECK(a.strahler_order == b.strahler_order);
    CHECK(a.shreve_magnitude == b.shreve_magnitude);
    CHECK(a.points_m == b.points_m);  // empty for the through-lake connector
    CHECK(a.discharge_m3_s == b.discharge_m3_s);
    CHECK(a.width_m == b.width_m);
    CHECK(a.depth_m == b.depth_m);
    CHECK(a.speed_m_s == b.speed_m_s);
  }
}

TEST_CASE("an empty river graph round-trips to zero nodes and edges",
          "[artifact]") {
  TempDir dir("empty");
  const RiverGraph g;  // default: no nodes, no edges
  const std::string path = dir.str() + "/rivers.bin";

  std::string err;
  REQUIRE(write_river_graph(path, g, &err));
  const auto loaded = read_river_graph(path, &err);
  REQUIRE(loaded.has_value());
  CHECK(loaded->nodes.empty());
  CHECK(loaded->edges.empty());
}

TEST_CASE("a truncated rivers.bin is rejected, not silently misread",
          "[artifact]") {
  TempDir dir("truncated");
  const RiverGraph g = make_graph();
  const std::string path = dir.str() + "/rivers.bin";

  std::string err;
  REQUIRE(write_river_graph(path, g, &err));

  const auto full_size = std::filesystem::file_size(path);
  REQUIRE(full_size > 4);
  std::filesystem::resize_file(path, full_size / 2);  // lands mid-record

  err.clear();
  const auto loaded = read_river_graph(path, &err);
  CHECK_FALSE(loaded.has_value());
  CHECK_FALSE(err.empty());
}

TEST_CASE("a file with the wrong magic is rejected", "[artifact]") {
  TempDir dir("badmagic");
  const std::string path = dir.str() + "/rivers.bin";
  {
    std::ofstream f(path, std::ios::binary);
    const uint32_t not_magic = 0xDEADBEEFu;
    f.write(reinterpret_cast<const char*>(&not_magic), sizeof(not_magic));
  }
  std::string err;
  CHECK_FALSE(read_river_graph(path, &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("magic"));
}
