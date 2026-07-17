// Tests for the authored-map loader (authored_map.cpp): the map_meta.json
// validation contract and the two image decodes.
//
// The loader's whole job is to fail LOUDLY on a malformed or mis-scaled asset
// rather than render a silently-wrong map, so most of these assert rejection.
// The biome-order group in particular guards the label->enum contract: the biome
// image stores raw Biome enum values, so an asset whose biome ordering disagrees
// with the engine would texture every biome wrongly with no error anywhere.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "badlands_assets.h"
#include "mapgen/authored_map.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"

using namespace badlands::mapgen;
namespace fs = std::filesystem;

namespace {

// Writes map_meta.json into a scratch directory; the image loaders write their
// PNGs alongside it. Removed on scope exit.
struct TempMapDir {
  fs::path dir;
  explicit TempMapDir(const std::string& meta_json) {
    dir = fs::temp_directory_path() /
          ("badlands_am_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(dir);
    std::ofstream(dir / "map_meta.json") << meta_json;
  }
  ~TempMapDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

constexpr const char* kEnumOrder =
    R"(["lake","swamp","forest","plains","hills","mountain"])";

// A complete, valid manifest. width/height must be a multiple of
// kSamplesPerBlock; meters_per_sample must equal kMetersPerSample.
std::string meta(const std::string& biomes = kEnumOrder, int w = 8, double mps = 1.0,
                 double lo = -8.0, double hi = 128.0) {
  return "{\"width\":" + std::to_string(w) + ",\"height\":8,\"meters_per_sample\":" +
         std::to_string(mps) + ",\"height_min_m\":" + std::to_string(lo) +
         ",\"height_max_m\":" + std::to_string(hi) + ",\"biomes\":" + biomes + "}";
}

bool meta_ok(const std::string& m) {
  TempMapDir d(m);
  AuthoredMapMeta out;
  std::string err;
  return read_authored_meta(d.dir.string(), out, err);
}

}  // namespace

// --- read_authored_meta: happy path + one regression guard per validation -----

TEST_CASE("read_authored_meta: accepts a complete valid manifest") {
  TempMapDir d(meta());
  AuthoredMapMeta out;
  std::string err;
  REQUIRE(read_authored_meta(d.dir.string(), out, err));
  CHECK(err.empty());
  CHECK(out.width == 8);
  CHECK(out.height == 8);
  CHECK(out.height_min_m == Catch::Approx(-8.0));
  CHECK(out.height_max_m == Catch::Approx(128.0));
  // png paths are derived from the width.
  CHECK(out.heights_png == (d.dir / "heights_8.png").string());
  CHECK(out.biome_png == (d.dir / "biome_8.png").string());
}

TEST_CASE("read_authored_meta: missing file is an error, not a crash") {
  AuthoredMapMeta out;
  std::string err;
  CHECK_FALSE(read_authored_meta("/nonexistent/authored/dir", out, err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("read_authored_meta: rejects non-positive dimensions") {
  CHECK_FALSE(meta_ok(meta(kEnumOrder, /*w*/ 0)));
}

TEST_CASE("read_authored_meta: rejects a size not divisible by kSamplesPerBlock") {
  // 10 is not a multiple of 4 -- the remainder would fall off the block grid.
  CHECK_FALSE(meta_ok(meta(kEnumOrder, /*w*/ 10)));
}

TEST_CASE("read_authored_meta: rejects an inverted height range") {
  CHECK_FALSE(meta_ok(meta(kEnumOrder, 8, 1.0, /*lo*/ 50.0, /*hi*/ 10.0)));
}

TEST_CASE("read_authored_meta: rejects a meters_per_sample that disagrees with the engine") {
  CHECK_FALSE(meta_ok(meta(kEnumOrder, 8, /*mps*/ 2.0)));
}

// --- the biome-order contract (finding #2): RED before the fix, GREEN after ----

TEST_CASE("read_authored_meta: rejects biomes in the wrong order") {
  // lake and swamp swapped: label 0 would texture as swamp, etc.
  CHECK_FALSE(
      meta_ok(meta(R"(["swamp","lake","forest","plains","hills","mountain"])")));
}

TEST_CASE("read_authored_meta: rejects a short biomes array") {
  CHECK_FALSE(meta_ok(meta(R"(["lake","swamp","forest","plains","hills"])")));
}

TEST_CASE("read_authored_meta: rejects a missing biomes array") {
  CHECK_FALSE(meta_ok(
      R"({"width":8,"height":8,"meters_per_sample":1.0,"height_min_m":-8.0,"height_max_m":128.0})"));
}

// --- load_authored_biome: raw enum values in, out-of-range rejected -----------

TEST_CASE("load_authored_biome: reads enum values and rejects out-of-range") {
  TempMapDir d(meta());
  AuthoredMapMeta m;
  std::string err;
  REQUIRE(read_authored_meta(d.dir.string(), m, err));

  // 8x8 grayscale-style RGBA8: R=G=B=label, cycling through the valid biomes.
  std::vector<uint8_t> px(8 * 8 * 4, 0);
  for (int i = 0; i < 8 * 8; ++i) {
    const uint8_t v = static_cast<uint8_t>(i % kBiomeCount);
    px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
    px[i * 4 + 3] = 255;
  }
  badlands_write_png(m.biome_png.c_str(), px.data(), 8, 8);

  Field2D<uint8_t> b;
  REQUIRE(load_authored_biome(m, b, err));
  CHECK(b.width == 8);
  CHECK(b.at(0, 0) == 0);
  CHECK(b.at(kBiomeCount - 1, 0) == kBiomeCount - 1);

  // A label past the enum must be rejected, not clamped or ignored.
  px[0] = static_cast<uint8_t>(kBiomeCount + 2);
  badlands_write_png(m.biome_png.c_str(), px.data(), 8, 8);
  Field2D<uint8_t> b2;
  CHECK_FALSE(load_authored_biome(m, b2, err));
}

// --- load_authored_heights: 16-bit decode + affine to meters ------------------

TEST_CASE("load_authored_heights: 16-bit decode maps through the meta affine") {
  TempMapDir d(meta());  // height range [-8, 128] -> span 136 m
  AuthoredMapMeta m;
  std::string err;
  REQUIRE(read_authored_meta(d.dir.string(), m, err));

  std::vector<uint16_t> luma(8 * 8, 0);
  luma[0] = 0;           // -> height_min
  luma[1] = 65535;       // -> height_max
  luma[2] = 2000;        // -> min + 2000/65535 * span
  badlands_write_png16(m.heights_png.c_str(), luma.data(), 8, 8);

  Field2D<float> h;
  REQUIRE(load_authored_heights(m, h, err));
  const float span = m.height_max_m - m.height_min_m;
  CHECK(h.at(0, 0) == Catch::Approx(m.height_min_m));
  CHECK(h.at(1, 0) == Catch::Approx(m.height_max_m));
  CHECK(h.at(2, 0) ==
        Catch::Approx(m.height_min_m + 2000.0f / 65535.0f * span).epsilon(1e-4));
}

TEST_CASE("load_authored_heights: rejects an image whose size disagrees with the meta") {
  TempMapDir d(meta());  // declares 8x8, so it looks for heights_8.png
  AuthoredMapMeta m;
  std::string err;
  REQUIRE(read_authored_meta(d.dir.string(), m, err));

  std::vector<uint16_t> luma(4 * 4, 0);  // wrong dimensions
  badlands_write_png16(m.heights_png.c_str(), luma.data(), 4, 4);

  Field2D<float> h;
  CHECK_FALSE(load_authored_heights(m, h, err));
}
