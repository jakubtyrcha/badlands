// The terrain-net bundle loader: the raw sidecar, the cover translation, the
// nodata prepass and the derived fields.
//
// Same hard rule as the other provider suites: every fixture is a synthetic
// bundle the test writes itself. No test here reads a fetched sample -- those
// are 184 MB, gitignored, and belong to another repo.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "mapgen/cover.hpp"
#include "mapgen/terrain_class.hpp"
#include "mapgen/terrain_net_patch_source.hpp"

using namespace badlands::mapgen;

namespace {

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& tag) {
    path = std::filesystem::temp_directory_path() /
           ("badlands_tnet_" + tag + "_" + std::to_string(::getpid()));
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
void write_flat(const std::filesystem::path& p, const std::vector<T>& v) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(v.data()),
          static_cast<std::streamsize>(v.size() * sizeof(T)));
}

// A minimal but complete bundle. Rows are written NORTH TO SOUTH, as the real
// writer does, so the loader's flip is genuinely exercised.
struct Bundle {
  TempDir dir;
  int w, h;
  std::vector<float> height;
  std::vector<uint8_t> landcover;

  Bundle(const std::string& tag, int w_, int h_)
      : dir(tag), w(w_), h(h_),
        height(static_cast<size_t>(w_) * h_, 100.0f),
        landcover(static_cast<size_t>(w_) * h_, 30) {}

  void write(double res_m = 1.0, bool landcover_present = true,
             const char* detail_class = "tor_blockfield") {
    write_flat(dir.path / "height.r32", height);
    if (landcover_present) write_flat(dir.path / "landcover.r8", landcover);
    std::ofstream(dir.path / "raw.json")
        << "{\"width\": " << w << ", \"height\": " << h
        << ", \"res_m\": " << res_m
        << ", \"height_format\": \"float32_le\", \"height_nodata\": -9999.0"
        << ", \"landcover_present\": " << (landcover_present ? "true" : "false")
        << ", \"row_order\": \"north_to_south\"}";
    if (detail_class) {
      std::ofstream(dir.path / "manifest.json")
          << "{\"area\": {\"key\": \"test\", \"name\": \"Test Area\", "
          << "\"detail_class\": \"" << detail_class << "\"}}";
    }
  }

  // (x, y) in the bundle's own north-to-south row order.
  void set(int x, int y, float z, uint8_t lc) {
    height[static_cast<size_t>(y) * w + x] = z;
    landcover[static_cast<size_t>(y) * w + x] = lc;
  }
};

}  // namespace

TEST_CASE("a bundle loads at its own geometry", "[terrain_net]") {
  Bundle b("geometry", 64, 64);
  b.write(/*res_m=*/0.5);

  std::string err;
  const auto src = LoadTerrainNetPatchSource(b.dir.str(), &err);
  REQUIRE(src);
  CHECK(err.empty());

  // The request is echoed, not honoured -- a survey is a finished artifact.
  CHECK(src->native_request().resolution == 64);
  CHECK(src->native_request().world_size_m == Catch::Approx(32.0f));

  const PatchData p = src->Fetch(PatchRequest{{0, 0}, 999.0f, 999});
  CHECK(p.height.width == 64);
  CHECK(p.texel_m == Catch::Approx(0.5f));
  CHECK(p.terrain_class == TerrainClass::TorBlockfield);
}

TEST_CASE("a non-square bundle crops to the largest centred square",
          "[terrain_net]") {
  // Real bundles are whatever the survey tiling produced (1088x1072 for
  // lake-district), but PatchRequest carries one resolution.
  Bundle b("nonsquare", 40, 32);
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});
  CHECK(p.height.width == 32);
  CHECK(p.height.height == 32);
  CHECK(p.cover.width == 32);
}

TEST_CASE("WorldCover codes translate into badlands' own vocabulary",
          "[terrain_net]") {
  Bundle b("cover", 16, 16);
  b.set(0, 0, 100.0f, 10);   // tree
  b.set(1, 0, 100.0f, 20);   // shrub
  b.set(2, 0, 100.0f, 40);   // crop
  b.set(3, 0, 100.0f, 60);   // bare
  b.set(4, 0, 100.0f, 100);  // moss
  b.set(5, 0, 100.0f, 77);   // not a WorldCover class at all
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});

  // Row 0 of the bundle is NORTH, so it lands at the far edge of the lattice.
  const int y = p.cover.height - 1;
  CHECK(p.cover.at(0, y) == uint8_t(Cover::Tree));
  CHECK(p.cover.at(1, y) == uint8_t(Cover::Shrub));
  CHECK(p.cover.at(2, y) == uint8_t(Cover::Crop));
  CHECK(p.cover.at(3, y) == uint8_t(Cover::Bare));
  CHECK(p.cover.at(4, y) == uint8_t(Cover::Moss));
  // Unrecognised is Unknown, never a plausible guess.
  CHECK(p.cover.at(5, y) == uint8_t(Cover::Unknown));
  // Shrub, crop and grass stay DISTINCT -- the whole reason this reads
  // landcover.r8 rather than the material raster, which collapses all three.
  CHECK(p.cover.at(1, y) != p.cover.at(2, y));
  CHECK(p.cover.at(2, y) != uint8_t(Cover::Grass));
}

TEST_CASE("rows load north-to-south, not mirrored", "[terrain_net]") {
  Bundle b("roworder", 8, 8);
  for (int x = 0; x < 8; ++x) b.set(x, 0, 200.0f, 30);  // the NORTH edge is high
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});
  CHECK(p.height.at(4, 7) == Catch::Approx(200.0f));
  CHECK(p.height.at(4, 0) == Catch::Approx(100.0f));
}

TEST_CASE("nodata is filled, and the fill is reported not hidden",
          "[terrain_net]") {
  Bundle b("nodata", 32, 32);
  b.set(10, 10, -9999.0f, 30);
  b.set(10, 11, -9999.0f, 30);
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});

  for (float z : p.height.data) CHECK(z == Catch::Approx(100.0f));
  CHECK(src->nodata_fraction() == Catch::Approx(2.0f / (32.0f * 32.0f)));
}

TEST_CASE("a hole in the DEM does not erase the cover observed over it",
          "[terrain_net]") {
  // Cover comes from satellite land cover, which is a wholly independent
  // observation and stays valid wherever the DEM happened to have a hole.
  //
  // This is also a lake-eater if got wrong: several LiDAR programmes leave open
  // water as outright nodata, and Cover::Water is the whole of the water
  // derivation's extent signal -- so marking filled texels Unknown would wipe
  // the mask exactly where the water is.
  Bundle b("nodata_water", 48, 48);
  for (int y = 8; y < 40; ++y)
    for (int x = 8; x < 40; ++x) b.set(x, y, -9999.0f, 80);  // water, no return
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});

  CHECK(p.cover.at(24, 24) == uint8_t(Cover::Water));
  REQUIRE(p.lakes.size() == 1);
  CHECK(p.water_depth.at(24, 24) > 0.0f);
}

TEST_CASE("a sidecar key of the wrong type fails loudly, not fatally",
          "[terrain_net]") {
  // value() throws on a key that EXISTS with the wrong type, and this function
  // promises a nullptr with a reason rather than an exception escaping into
  // main, which has no handler.
  Bundle b("badtype", 16, 16);
  b.write();
  std::ofstream(b.dir.path / "raw.json")
      << "{\"width\": 16, \"height\": 16, \"res_m\": 1.0, "
         "\"landcover_present\": true, \"row_order\": 3}";

  std::string err;
  CHECK_FALSE(LoadTerrainNetPatchSource(b.dir.str(), &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("a bundle that is mostly hole is refused, not extrapolated",
          "[terrain_net]") {
  Bundle b("mostlyhole", 32, 32);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 32; ++x) b.set(x, y, -9999.0f, 30);
  }
  b.write();

  std::string err;
  CHECK_FALSE(LoadTerrainNetPatchSource(b.dir.str(), &err));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("nodata"));
}

TEST_CASE("a bundle without land cover is refused on the sidecar's own word",
          "[terrain_net]") {
  Bundle b("nolandcover", 16, 16);
  b.write(1.0, /*landcover_present=*/false);

  std::string err;
  CHECK_FALSE(LoadTerrainNetPatchSource(b.dir.str(), &err));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("land cover"));
}

TEST_CASE("a raster whose size contradicts the sidecar is refused",
          "[terrain_net]") {
  // The arrays are headerless, so this is the only thing between a wrong
  // sidecar and a map silently reinterpreted at the wrong stride.
  Bundle b("badsize", 16, 16);
  b.write();
  std::ofstream(b.dir.path / "raw.json")
      << "{\"width\": 32, \"height\": 32, \"res_m\": 1.0, "
         "\"landcover_present\": true, \"row_order\": \"north_to_south\"}";

  std::string err;
  CHECK_FALSE(LoadTerrainNetPatchSource(b.dir.str(), &err));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("expected"));
}

TEST_CASE("a malformed sidecar fails loudly", "[terrain_net]") {
  TempDir dir("badjson");
  std::ofstream(dir.path / "raw.json") << "{ not json";

  std::string err;
  CHECK_FALSE(LoadTerrainNetPatchSource(dir.str(), &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("an unlabelled bundle loads with an Unknown terrain class",
          "[terrain_net]") {
  // A --bbox fetch is legitimately unlabelled; that costs a palette
  // specialisation, not the patch.
  Bundle b("unlabelled", 16, 16);
  b.write(1.0, true, /*detail_class=*/nullptr);

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  CHECK(src->Fetch({}).terrain_class == TerrainClass::Unknown);
}

TEST_CASE("the derived fields are all present and sized", "[terrain_net]") {
  // The contract says a patch HAS soil and water, whatever produced them.
  Bundle b("derived", 32, 32);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) b.set(x, y, 100.0f + 0.5f * x, 30);
  }
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});

  const size_t n = p.height.size();
  CHECK(p.level.size() == n);
  CHECK(p.soil.size() == n);
  CHECK(p.water_depth.size() == n);
  CHECK(p.lake_id.size() == n);
  // A slope of atan(0.5) ~= 26.6 deg, so soil is thin but present everywhere.
  for (float s : p.soil.data) CHECK(s > 0.0f);
  // Rivers are deferred, and their absence must be visible rather than faked.
  CHECK(p.rivers.nodes.empty());
  CHECK(p.elevation_range.max_m > p.elevation_range.min_m);
}

TEST_CASE("observed water becomes a flat lake over a carved bed",
          "[terrain_net]") {
  Bundle b("lake", 48, 48);
  // A plate of water 1 m below the surrounding plain, as a real hydro-flattened
  // DTM presents it.
  for (int y = 8; y < 40; ++y) {
    for (int x = 8; x < 40; ++x) b.set(x, y, 99.0f, 80);  // WorldCover water
  }
  b.write();

  const auto src = LoadTerrainNetPatchSource(b.dir.str());
  REQUIRE(src);
  const PatchData p = src->Fetch({});

  REQUIRE(p.lakes.size() == 1);
  CHECK(p.water_depth.at(24, 24) > 0.0f);
  CHECK(p.level.at(20, 20) == p.level.at(28, 28));  // exactly flat
  CHECK(p.water_depth.at(0, 0) == 0.0f);            // dry outside
}

TEST_CASE("IsTerrainNetBundle recognises a bundle and nothing else",
          "[terrain_net]") {
  Bundle b("sniff", 8, 8);
  b.write();
  CHECK(IsTerrainNetBundle(b.dir.str()));

  TempDir other("notabundle");
  std::ofstream(other.path / "map.txt") << "resolution 8\n";
  CHECK_FALSE(IsTerrainNetBundle(other.str()));
}
