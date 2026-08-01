// Tests for the on-disk map form: the level convention, lake derivation, and
// the size checks that stand between a wrong manifest and a silently
// reinterpreted map (the rasters are headerless).

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/map_io.hpp"

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

TEST_CASE("load_map round-trips a written directory", "[map_io]") {
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
  const auto art = load_map(dir.str(), &err);
  REQUIRE(art.has_value());
  CHECK(err.empty());
  CHECK(art->heightmap.width == n);
  CHECK(art->heightmap.height == n);
  CHECK(art->heightmap.data == height);
  CHECK(art->biome.data == biome);
  // bedrock is set to the heightmap: mapview reads it only for dimensions.
  CHECK(art->bedrock.width == n);
  REQUIRE(art->lakes.size() == 1);
  CHECK(art->lakes[0].max_depth_m == 10.0f);
  CHECK(art->water_depth.data[20] == 10.0f);
  CHECK(art->lake_id.data[20] == 0);
  CHECK(art->lake_id.data[0] == -1);
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
  const auto art = load_map(dir.str(), &err);
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
  CHECK_FALSE(load_map(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("biome.u8"));
}

TEST_CASE("a missing or malformed manifest is reported, not guessed",
          "[map_io]") {
  TempDir dir("nomanifest");
  std::string err;
  CHECK_FALSE(load_manifest(dir.str(), &err).has_value());
  CHECK_FALSE(err.empty());

  std::ofstream(dir.str() + "/map.txt") << "world_size_m 64\n";  // no resolution
  err.clear();
  CHECK_FALSE(load_manifest(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("resolution"));
}

TEST_CASE("unknown manifest keys are ignored so the writer can add fields",
          "[map_io]") {
  TempDir dir("extrakeys");
  std::ofstream(dir.str() + "/map.txt")
      << "# a comment\nresolution 4\nworld_size_m 32\nsource protogen tag=x\n"
         "future_key 1234\n";
  std::string err;
  const auto man = load_manifest(dir.str(), &err);
  REQUIRE(man.has_value());
  CHECK(man->resolution == 4);
  CHECK(man->world_size_m == 32.0f);
  CHECK(man->source == "protogen tag=x");
}
