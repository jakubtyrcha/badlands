// Pure-CPU tests for the biome -> material-pack manifest resolution. The
// mapping is order-critical: index i of the returned vector becomes layer i of
// the terrain texture arrays, and the shader indexes those layers by the Biome
// enum value baked into the mesh. A silent mis-map would texture every biome
// wrongly with no error anywhere, so the resolver is keyed by name and every
// biome is mandatory.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapview/biome_manifest.hpp"

using namespace badlands;

namespace {

// Writes `contents` to a scratch file and removes it on scope exit.
struct TempManifest {
  std::string path;
  explicit TempManifest(const std::string& contents, const char* name) {
    path = std::string(std::tmpnam(nullptr)) + name + ".json";
    std::ofstream f(path);
    f << contents;
  }
  ~TempManifest() { std::remove(path.c_str()); }
};

constexpr const char* kComplete = R"({
  "lake":     "packs/lake",
  "swamp":    "packs/swamp",
  "forest":   "packs/forest",
  "plains":   "packs/plains",
  "hills":    "packs/hills",
  "mountain": "packs/mountain"
})";

}  // namespace

TEST_CASE("ResolveBiomePacks: resolves every biome in enum order") {
  TempManifest m(kComplete, "complete");
  std::vector<std::string> packs;
  REQUIRE(ResolveBiomePacks(m.path, packs));

  // Order is the contract: index == Biome enum value == array layer index.
  REQUIRE(packs.size() == static_cast<size_t>(mapgen::kBiomeCount));
  CHECK(packs[static_cast<int>(mapgen::Biome::Lake)] == "packs/lake");
  CHECK(packs[static_cast<int>(mapgen::Biome::Swamp)] == "packs/swamp");
  CHECK(packs[static_cast<int>(mapgen::Biome::Forest)] == "packs/forest");
  CHECK(packs[static_cast<int>(mapgen::Biome::Plains)] == "packs/plains");
  CHECK(packs[static_cast<int>(mapgen::Biome::Hills)] == "packs/hills");
  CHECK(packs[static_cast<int>(mapgen::Biome::Mountain)] == "packs/mountain");
}

TEST_CASE("ResolveBiomePacks: key order in the file does not matter") {
  // Name-keyed, so shuffling the JSON must not shuffle the layers.
  TempManifest m(R"({
    "hills":  "packs/hills",
    "mountain": "packs/mountain",
    "forest": "packs/forest",
    "lake":   "packs/lake",
    "plains": "packs/plains",
    "swamp":  "packs/swamp"
  })",
                 "shuffled");
  std::vector<std::string> packs;
  REQUIRE(ResolveBiomePacks(m.path, packs));
  CHECK(packs[static_cast<int>(mapgen::Biome::Lake)] == "packs/lake");
  CHECK(packs[static_cast<int>(mapgen::Biome::Hills)] == "packs/hills");
}

TEST_CASE("ResolveBiomePacks: a missing biome is an error, not a short list") {
  TempManifest m(R"({
    "lake": "packs/lake", "swamp": "packs/swamp",
    "forest": "packs/forest", "plains": "packs/plains",
    "mountain": "packs/mountain"
  })",
                 "nohills");  // hills omitted
  std::vector<std::string> packs;
  CHECK_FALSE(ResolveBiomePacks(m.path, packs));
  CHECK(packs.empty());  // must not leave a partial mapping behind
}

TEST_CASE("ResolveBiomePacks: a non-string entry is an error") {
  TempManifest m(R"({
    "lake": 42, "swamp": "packs/swamp", "forest": "packs/forest",
    "plains": "packs/plains", "hills": "packs/hills",
    "mountain": "packs/mountain"
  })",
                 "notstring");
  std::vector<std::string> packs;
  CHECK_FALSE(ResolveBiomePacks(m.path, packs));
  CHECK(packs.empty());
}

TEST_CASE("ResolveBiomePacks: unparseable JSON is an error") {
  TempManifest m("{ this is not json", "broken");
  std::vector<std::string> packs;
  CHECK_FALSE(ResolveBiomePacks(m.path, packs));
  CHECK(packs.empty());
}

TEST_CASE("ResolveBiomePacks: a missing file is an error") {
  std::vector<std::string> packs;
  CHECK_FALSE(ResolveBiomePacks("/nonexistent/terrain_biomes.json", packs));
  CHECK(packs.empty());
}

TEST_CASE("ResolveBiomePacks: the shipped manifest resolves") {
  // Guards the real asset: every biome present, all pack dirs non-empty.
  std::vector<std::string> packs;
  REQUIRE(ResolveBiomePacks("assets/materials/terrain_biomes.json", packs));
  REQUIRE(packs.size() == static_cast<size_t>(mapgen::kBiomeCount));
  for (const std::string& p : packs) CHECK_FALSE(p.empty());
}
