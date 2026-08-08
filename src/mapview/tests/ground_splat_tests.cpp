// Pure-CPU tests for the patch -> ground splat raster the cluster terrain
// samples by world XZ, and for the slot -> pack manifest.
//
// Three invariants carry over from the biome splat this replaced, because a
// wrong splat breaks them silently: slot index == the terrain array's layer
// index (or every slot wears the wrong texture), weights sum to full (or the
// blend darkens), and at most two layers per texel (or the fragment shader's
// fetch count is unbounded).
//
// What is NOT pinned here is the derivation's tuning. Those constants are
// provisional pending the material-variation work, and a test asserting that
// a 38-degree slope is 61% rock would have to be rewritten the moment anyone
// improved it. The tests below assert DIRECTION -- steeper is rockier, wetter
// is peatier -- which is what the derivation actually claims.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "mapgen/cover.hpp"
#include "mapview/ground_splat.hpp"

using namespace badlands;
using mapgen::Cover;

namespace {

// A patch tilted by `deg` along +x, uniformly covered and uniformly soiled.
mapgen::PatchData MakePatch(int n, float deg, Cover cover, float soil_m,
                            float texel_m = 1.0f) {
  mapgen::PatchData p;
  p.texel_m = texel_m;
  p.height = mapgen::Field2D<float>(n, n);
  p.cover = mapgen::Field2D<uint8_t>(n, n, static_cast<uint8_t>(cover));
  p.soil = mapgen::Field2D<float>(n, n, soil_m);
  p.water_depth = mapgen::Field2D<float>(n, n, 0.0f);
  const float rise = std::tan(deg * 3.14159265358979f / 180.0f) * texel_m;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) p.height.at(x, y) = static_cast<float>(x) * rise;
  return p;
}

int Weight(const GroundSplat& s, GroundSlot slot, int x, int y) {
  const size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
  const int k = static_cast<int>(slot);
  return k < 4 ? s.slots0[i + k] : s.slots1[i + (k - 4)];
}

int TotalWeight(const GroundSplat& s, int x, int y) {
  int total = 0;
  for (int k = 0; k < kGroundSlotCount; ++k)
    total += Weight(s, static_cast<GroundSlot>(k), x, y);
  return total;
}

int NonZeroSlots(const GroundSplat& s, int x, int y) {
  int n = 0;
  for (int k = 0; k < kGroundSlotCount; ++k)
    n += (Weight(s, static_cast<GroundSlot>(k), x, y) > 0);
  return n;
}

struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const std::string& body) {
    path = std::filesystem::temp_directory_path() /
           ("badlands_ground_" + std::to_string(::getpid()) + ".json");
    std::ofstream(path) << body;
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  std::string str() const { return path.string(); }
};

}  // namespace

// ------------------------------------------------------------- invariants ---

TEST_CASE("an empty patch yields an empty splat", "[splat]") {
  REQUIRE(BuildGroundSplat(mapgen::PatchData{}).empty());
}

TEST_CASE("weights sum to full at every texel", "[splat]") {
  // A short sum would not darken the blend -- the shader renormalises by what
  // it receives -- but an exact sum keeps the invariant checkable and the
  // bilinear interpolation between neighbouring texels energy-preserving.
  const mapgen::PatchData p = MakePatch(24, 20.0f, Cover::Grass, 1.0f);
  const GroundSplat s = BuildGroundSplat(p);
  REQUIRE_FALSE(s.empty());
  for (int y = 0; y < s.height; ++y)
    for (int x = 0; x < s.width; ++x) REQUIRE(TotalWeight(s, x, y) == 255);
}

TEST_CASE("at most two layers are non-zero per texel", "[splat]") {
  // Every extra non-zero slot is another 6 texture fetches in the fragment
  // shader, and bilinear filtering of a 2-weight texel can already light up 4.
  mapgen::PatchData p = MakePatch(32, 25.0f, Cover::Grass, 1.0f);
  // A cover boundary and a slope break in the same patch, so the cull has
  // something to actually cull.
  for (int y = 0; y < 32; ++y)
    for (int x = 16; x < 32; ++x) {
      p.cover.at(x, y) = static_cast<uint8_t>(Cover::Tree);
      p.height.at(x, y) += static_cast<float>(x - 16) * 0.8f;
    }
  const GroundSplat s = BuildGroundSplat(p);
  for (int y = 0; y < s.height; ++y)
    for (int x = 0; x < s.width; ++x) REQUIRE(NonZeroSlots(s, x, y) <= 2);
}

TEST_CASE("a texel nothing claims still gets a material", "[splat]") {
  // An all-zero weight vector renormalises to a black surface, so a slot has to
  // win even when the derivation has nothing to say.
  mapgen::PatchData p;
  p.texel_m = 1.0f;
  p.height = mapgen::Field2D<float>(8, 8, 0.0f);
  // No cover, no soil, no water rasters at all.
  const GroundSplat s = BuildGroundSplat(p);
  REQUIRE_FALSE(s.empty());
  for (int y = 0; y < s.height; ++y)
    for (int x = 0; x < s.width; ++x) REQUIRE(TotalWeight(s, x, y) == 255);
}

// ------------------------------------------------------------- direction ---

TEST_CASE("steeper ground reads rockier", "[splat]") {
  // Deep soil throughout, so this isolates SLOPE. At a shallower depth the
  // ground is legitimately part-stripped and shows rock at any gradient, which
  // is the separate claim the next case makes.
  const auto rock_at = [](float deg) {
    const GroundSplat s = BuildGroundSplat(MakePatch(24, deg, Cover::Grass, 4.0f));
    return Weight(s, GroundSlot::BareRock, 12, 12);
  };
  REQUIRE(rock_at(5.0f) == 0);
  REQUIRE(rock_at(60.0f) > rock_at(30.0f));
  REQUIRE(rock_at(30.0f) >= rock_at(5.0f));
}

TEST_CASE("thin soil exposes rock on ground that is not steep", "[splat]") {
  // The physically interesting half: exposure is about where soil CANNOT stay,
  // and stripped ground qualifies even at a gentle gradient.
  const auto rock_at = [](float soil) {
    const GroundSplat s = BuildGroundSplat(MakePatch(24, 12.0f, Cover::Grass, soil));
    return Weight(s, GroundSlot::BareRock, 12, 12) +
           Weight(s, GroundSlot::StonyGround, 12, 12);
  };
  REQUIRE(rock_at(0.1f) > rock_at(4.0f));
}

TEST_CASE("cover picks the vegetated slot", "[splat]") {
  struct Case { Cover cover; GroundSlot slot; };
  for (const Case c : {Case{Cover::Tree, GroundSlot::ForestFloor},
                       Case{Cover::Shrub, GroundSlot::Heath},
                       Case{Cover::Moss, GroundSlot::Heath},
                       Case{Cover::Wetland, GroundSlot::Peat},
                       Case{Cover::Grass, GroundSlot::Turf},
                       Case{Cover::Crop, GroundSlot::Turf}}) {
    const GroundSplat s = BuildGroundSplat(MakePatch(24, 3.0f, c.cover, 3.0f));
    INFO("cover " << mapgen::cover_name(c.cover));
    REQUIRE(Weight(s, c.slot, 12, 12) > 128);
  }
}

TEST_CASE("standing water overrides the slope entirely", "[splat]") {
  // A lake bed is silt whatever the ground beneath it is doing.
  mapgen::PatchData p = MakePatch(24, 50.0f, Cover::Grass, 0.0f);
  for (int y = 8; y < 16; ++y)
    for (int x = 8; x < 16; ++x) p.water_depth.at(x, y) = 2.0f;
  const GroundSplat s = BuildGroundSplat(p);
  REQUIRE(Weight(s, GroundSlot::Silt, 12, 12) > 128);
  REQUIRE(Weight(s, GroundSlot::BareRock, 2, 12) > 0);  // dry ground unaffected
}

TEST_CASE("the derivation is resolution independent", "[splat]") {
  // The same terrain sampled more finely must reach the same material -- slope
  // is per METRE, and the blur radius is in metres for the same reason.
  const GroundSplat coarse =
      BuildGroundSplat(MakePatch(32, 45.0f, Cover::Grass, 0.5f, 4.0f));
  const GroundSplat fine =
      BuildGroundSplat(MakePatch(128, 45.0f, Cover::Grass, 0.5f, 1.0f));
  REQUIRE(Weight(coarse, GroundSlot::BareRock, 16, 16) ==
          Weight(fine, GroundSlot::BareRock, 64, 64));
}

// -------------------------------------------------------------- manifest ---

namespace {
constexpr const char* kFullDefault = R"({
  "default": {
    "bare_rock": "d/rock", "scree": "d/scree", "stony_ground": "d/stony",
    "turf": "d/turf", "heath": "d/heath", "peat": "d/peat",
    "silt": "d/silt", "forest_floor": "d/forest"
  },
  "tor_blockfield": { "bare_rock": "tor/granite" }
})";
}  // namespace

TEST_CASE("slot index is the array layer index", "[manifest]") {
  // The whole point of resolving by NAME into a positional vector: a reordered
  // or renamed manifest entry must not silently bind the wrong texture.
  TempFile f(kFullDefault);
  std::vector<std::string> packs;
  REQUIRE(ResolveGroundPacks(f.str(), mapgen::TerrainClass::Unknown, packs));
  REQUIRE(packs.size() == kGroundSlotCount);
  REQUIRE(packs[static_cast<int>(GroundSlot::BareRock)] == "d/rock");
  REQUIRE(packs[static_cast<int>(GroundSlot::ForestFloor)] == "d/forest");
}

TEST_CASE("a terrain class overrides only the slots it names", "[manifest]") {
  // Partial blocks are what let a granite tor and a limestone dale differ in
  // their rock without restating the whole set.
  TempFile f(kFullDefault);
  std::vector<std::string> packs;
  REQUIRE(ResolveGroundPacks(f.str(), mapgen::TerrainClass::TorBlockfield, packs));
  REQUIRE(packs[static_cast<int>(GroundSlot::BareRock)] == "tor/granite");
  REQUIRE(packs[static_cast<int>(GroundSlot::Turf)] == "d/turf");
}

TEST_CASE("an unlabelled or unknown class still resolves", "[manifest]") {
  // A --bbox fetch is legitimately unlabelled, and a bundle from a newer
  // terrain-net may name a class this build has never heard of. Neither is a
  // reason to fail to render.
  TempFile f(kFullDefault);
  std::vector<std::string> packs;
  REQUIRE(ResolveGroundPacks(f.str(), mapgen::TerrainClass::KarstPavement, packs));
  REQUIRE(packs[static_cast<int>(GroundSlot::BareRock)] == "d/rock");
}

TEST_CASE("a manifest missing a slot fails loudly", "[manifest]") {
  TempFile f(R"({"default": {"bare_rock": "d/rock"}})");
  std::vector<std::string> packs;
  REQUIRE_FALSE(ResolveGroundPacks(f.str(), mapgen::TerrainClass::Unknown, packs));
  REQUIRE(packs.empty());
}

TEST_CASE("a manifest with no default block fails loudly", "[manifest]") {
  TempFile f(R"({"tor_blockfield": {"bare_rock": "tor/granite"}})");
  std::vector<std::string> packs;
  REQUIRE_FALSE(ResolveGroundPacks(f.str(), mapgen::TerrainClass::TorBlockfield, packs));
}

TEST_CASE("a missing or unparseable manifest fails loudly", "[manifest]") {
  std::vector<std::string> packs;
  REQUIRE_FALSE(ResolveGroundPacks("/nonexistent/terrain_ground.json",
                                   mapgen::TerrainClass::Unknown, packs));
  TempFile bad("{ not json");
  REQUIRE_FALSE(ResolveGroundPacks(bad.str(), mapgen::TerrainClass::Unknown, packs));
}

TEST_CASE("the shipped manifest resolves for every terrain class",
          "[manifest]") {
  // Guards the real asset file: a slot dropped from "default", or a typo in a
  // per-class block, would otherwise only show up as a failed launch.
  for (int c = 0; c < mapgen::kTerrainClassCount; ++c) {
    std::vector<std::string> packs;
    INFO("class " << mapgen::terrain_class_name(static_cast<mapgen::TerrainClass>(c)));
    REQUIRE(ResolveGroundPacks("assets/materials/terrain_ground.json",
                               static_cast<mapgen::TerrainClass>(c), packs));
    REQUIRE(packs.size() == kGroundSlotCount);
    for (const std::string& d : packs) {
      // material.json, not merely the directory. Only 15 of the 37 pack
      // directories carry one, and MaterialLibrary refuses a pack without it --
      // so a manifest naming a bare directory passes an existence check and
      // then fails at launch, which is exactly what this caught once.
      INFO("pack " << d);
      REQUIRE(std::filesystem::exists(d + "/material.json"));
    }
  }
}
