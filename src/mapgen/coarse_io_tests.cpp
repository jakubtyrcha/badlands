// Tests for the coarse (protogen) artifact's world.txt manifest -- the
// key/value idiom copied from patch_io's map.txt, giving the coarse dump's
// resolution/world size/soil quantiles somewhere to live besides argv.
//
// HARD RULE: no test here may run the coarse sim. Fixtures are hand-written
// manifests only.

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mapgen/coarse_io.hpp"

using namespace badlands::mapgen;

namespace {

// A scratch directory that cleans itself up, so a failing assertion cannot
// leak state into the next test. Same pattern as patch_io_tests.cpp's
// TempDir, with its own name prefix to avoid colliding with it.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / ("bl_coarse_io_" + name)) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

}  // namespace

TEST_CASE("coarse manifest round-trips exactly, including soil cutoffs",
          "[artifact]") {
  TempDir dir("roundtrip");
  CoarseManifest m;
  m.resolution = 512;
  m.world_size_m = 8192.0f;
  m.texel_m = m.world_size_m / static_cast<float>(m.resolution);
  m.seed = 123456789u;
  m.runoff_m_per_yr = 1.25f;
  m.steps = 3000;
  // Deliberately not round numbers -- an EXACT round trip has to survive the
  // text encode/decode, not merely a value that happens to print cleanly.
  m.soil_cut_mountain_m = 0.7345678f;
  m.soil_cut_hills_m = 3.14159274f;

  std::string err;
  REQUIRE(write_coarse_manifest(dir.str(), m, &err));
  CHECK(err.empty());

  const auto loaded = load_coarse_manifest(dir.str(), &err);
  REQUIRE(loaded.has_value());
  CHECK(err.empty());
  CHECK(loaded->resolution == m.resolution);
  CHECK(loaded->world_size_m == m.world_size_m);
  CHECK(loaded->texel_m == m.texel_m);
  CHECK(loaded->seed == m.seed);
  CHECK(loaded->runoff_m_per_yr == m.runoff_m_per_yr);
  CHECK(loaded->steps == m.steps);
  CHECK(loaded->soil_cut_mountain_m == m.soil_cut_mountain_m);
  CHECK(loaded->soil_cut_hills_m == m.soil_cut_hills_m);
}

TEST_CASE("an unknown key in world.txt is ignored so the writer can add fields",
          "[artifact]") {
  TempDir dir("extrakey");
  std::ofstream(dir.str() + "/world.txt")
      << "# a comment\nresolution 256\nworld_size_m 4096\n"
         "future_key 1234\nseed 7\n";
  std::string err;
  const auto m = load_coarse_manifest(dir.str(), &err);
  REQUIRE(m.has_value());
  CHECK(err.empty());
  CHECK(m->resolution == 256);
  CHECK(m->world_size_m == 4096.0f);
  CHECK(m->seed == 7u);
}

TEST_CASE("a missing or invalid resolution/world_size_m is rejected, not guessed",
          "[artifact]") {
  TempDir dir("invalid");

  // No world.txt at all.
  std::string err;
  CHECK_FALSE(load_coarse_manifest(dir.str(), &err).has_value());
  CHECK_FALSE(err.empty());

  // world_size_m present, resolution missing.
  std::ofstream(dir.str() + "/world.txt") << "world_size_m 4096\n";
  err.clear();
  CHECK_FALSE(load_coarse_manifest(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("resolution"));

  // resolution present, world_size_m invalid (<= 0).
  std::ofstream(dir.str() + "/world.txt") << "resolution 256\nworld_size_m 0\n";
  err.clear();
  CHECK_FALSE(load_coarse_manifest(dir.str(), &err).has_value());
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("world_size_m"));
}
