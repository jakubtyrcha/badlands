// badlands' forest content, now loaded from assets/foliage/temperate_forest.json.
//
// Two things are checked here, and the split is deliberate. The SHIPPED file is
// checked for structural invariants that any sane forest must hold (layers
// partition the model list, a variant is a genuinely different mesh, stature is
// ordered bush < sapling < canopy) -- NOT for exact numbers, because the file is
// the tuning surface and a test that fired on every density tweak would be the
// same friction the file exists to remove.
//
// The LOADER is then checked against deliberately broken files, since that is
// the new failure surface: a hand-edited file must fail loudly and name the
// field, never load half a forest.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "foliage/forest_type.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/visual/forest_catalog.hpp"

using namespace badlands;

namespace {

const TreeOptions* FindPreset(const std::vector<NamedTreeOptions>& catalog,
                              const std::string& name) {
  for (const NamedTreeOptions& n : catalog)
    if (n.name == name) return &n.options;
  return nullptr;
}

// Writes `text` to a scratch file and loads it, so a malformed-input case is a
// literal string in the test rather than a fixture to go hunting for.
bool LoadFromText(const std::string& text, ForestCatalog& out) {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / "badlands_forest_case.json";
  {
    std::ofstream f(p);
    f << text;
  }
  const bool ok = LoadForestCatalog(p.string(), out);
  std::filesystem::remove(p);
  return ok;
}

// A minimal well-formed forest, used as the base every broken case mutates.
//
// The JSON delimiter on these raw strings is load-bearing, not style: a preset
// name like "Oak (large)" contains the `)"` sequence that would otherwise
// terminate the literal mid-string.
std::string MinimalForest(const std::string& layer_body) {
  return R"JSON({
    "noise": { "clump_wavelength_m": 35.0, "clump_octaves": 3,
               "clump_lo": 0.30, "clump_hi": 0.55,
               "warp_amp_m": 4.0, "warp_wavelength_m": 12.0 },
    "layers": [ )JSON" + layer_body + R"JSON( ]
  })JSON";
}

const char* kGoodLayer = R"JSON({
  "grid_m": 6.0, "max_slope_deg": 32.0,
  "density": [1.0, 14.0, null, null],
  "scale_range": [0.85, 1.15],
  "edge_scale": 0.55, "edge_scale_depth_m": 25.0,
  "species": [
    { "preset": "Oak (large)", "variants": 2, "height_m": 22.0,
      "radius_m": 3.4, "weight": 1.0, "depth": [2.0, 10.0, null, null] }
  ]
})JSON";

}  // namespace

// ------------------------------------------------------- the shipped forest

TEST_CASE("The shipped forest file loads", "[foliage]") {
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));
  CHECK(fc.type.Valid());
  CHECK(fc.models.size() == fc.type.models.size());
  CHECK(fc.models.size() >= 8);       // enough variety to be worth instancing
  CHECK(fc.type.layers.size() >= 2);  // at least a canopy and an understory
}

TEST_CASE("Layer slices partition the model list exactly", "[foliage]") {
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  // Contiguous, in order, and covering every model with no gap or overlap --
  // a gap would mean a generated model nothing can ever place.
  size_t expected_first = 0;
  for (const foliage::FoliageLayer& l : fc.type.layers) {
    CHECK(l.first_model == expected_first);
    CHECK(l.model_count > 0);
    expected_first += l.model_count;
  }
  CHECK(expected_first == fc.type.models.size());
}

TEST_CASE("Variant 0 is the catalog tree the model viewer shows", "[foliage]") {
  // If variant 0 drifted from the preset, the viewer's single-tree preview
  // would show a tree that never appears in any forest.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();

  int checked = 0;
  for (const ForestModelSpec& m : fc.models) {
    if (m.debug_name.find(" v0") == std::string::npos) continue;
    const std::string preset_name =
        m.debug_name.substr(0, m.debug_name.size() - 3);
    const TreeOptions* preset = FindPreset(catalog, preset_name);
    REQUIRE(preset != nullptr);
    CHECK(m.options.seed == preset->seed);
    checked++;
  }
  CHECK(checked > 0);
}

TEST_CASE("Variants of one species are genuinely different meshes", "[foliage]") {
  // A variant is only worth its GPU slot if the skeleton actually differs.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  std::vector<std::string> names;
  for (const ForestModelSpec& m : fc.models)
    names.push_back(m.debug_name.substr(0, m.debug_name.rfind(" v")));

  // Seeds must be distinct WITHIN a preset -- that is what makes one variant
  // different from the next. Across presets a shared seed is harmless and does
  // occur upstream (Bush 1 and Bush 2 both ship ez-tree seed 45590): the seed
  // is not the identity, the whole TreeOptions is.
  std::map<std::string, std::set<uint32_t>> seeds_by_preset;
  std::map<std::string, int> count_by_preset;
  for (size_t i = 0; i < fc.models.size(); ++i) {
    seeds_by_preset[names[i]].insert(fc.models[i].options.seed);
    count_by_preset[names[i]]++;
  }
  for (const auto& [preset, seeds] : seeds_by_preset) {
    INFO("preset " << preset);
    CHECK(seeds.size() == static_cast<size_t>(count_by_preset[preset]));
  }

  for (size_t i = 0; i + 1 < fc.models.size(); ++i) {
    if (names[i] != names[i + 1]) continue;
    const std::vector<SkeletonBranch> a = BuildTreeSkeleton(fc.models[i].options);
    const std::vector<SkeletonBranch> b =
        BuildTreeSkeleton(fc.models[i + 1].options);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());

    bool differs = a.size() != b.size();
    if (!differs) {
      for (size_t k = 0; k < a.size() && !differs; ++k) {
        if (a[k].sections.size() != b[k].sections.size()) { differs = true; break; }
        for (size_t s = 0; s < a[k].sections.size(); ++s) {
          if (a[k].sections[s].origin != b[k].sections[s].origin) {
            differs = true;
            break;
          }
        }
      }
    }
    INFO("preset " << names[i] << " variants " << i << " and " << i + 1);
    CHECK(differs);
  }
}

TEST_CASE("Layers do not overlap in stature", "[foliage]") {
  // "sapling" and "canopy" stop meaning anything on screen if they are the same
  // size. Layers are ordered tallest-first in the file, so each must sit
  // strictly above the next.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  auto height_range = [&](const foliage::FoliageLayer& l) {
    float lo = 1e9f, hi = 0.0f;
    for (uint16_t k = 0; k < l.model_count; ++k) {
      const float h = fc.type.models[l.first_model + k].height_m;
      lo = std::min(lo, h);
      hi = std::max(hi, h);
    }
    return std::pair<float, float>{lo, hi};
  };

  for (size_t i = 0; i + 1 < fc.type.layers.size(); ++i) {
    const auto [lo_a, hi_a] = height_range(fc.type.layers[i]);
    const auto [lo_b, hi_b] = height_range(fc.type.layers[i + 1]);
    INFO("layer " << i << " [" << lo_a << "," << hi_a << "] vs layer " << i + 1
                  << " [" << lo_b << "," << hi_b << "]");
    CHECK(lo_a > hi_b);
  }

  // And every model has a usable footprint.
  for (const foliage::FoliageModel& m : fc.type.models) {
    CHECK(m.radius_m > 0.0f);
    CHECK(m.radius_m < m.height_m);
    CHECK(m.scale_range.x > 0.0f);
    CHECK(m.scale_range.y >= m.scale_range.x);
    CHECK(m.weight > 0.0f);
  }
}

TEST_CASE("A species' share does not depend on its variant count", "[foliage]") {
  // `weight` is documented as the species' share of its layer. The sampler
  // picks over MODELS, so handing each variant the full weight silently made a
  // species' actual share weight * variants. It was live in the shipped file:
  // Bush 1 (weight 1.00, 2 variants) took 59% of bushes against the 42% the
  // file reads as.
  //
  // Two species, identical weight, different variant counts -- their summed
  // model weight must come out equal.
  const std::string body = R"JSON({
    "grid_m": 6.0, "max_slope_deg": 32.0,
    "density": [0.0, 1.0, null, null],
    "scale_range": [0.9, 1.1],
    "edge_scale": 1.0, "edge_scale_depth_m": 1.0,
    "species": [
      { "preset": "Bush 1", "variants": 4, "height_m": 1.5, "radius_m": 0.9,
        "weight": 1.0, "depth": [0.0, 1.0, null, null] },
      { "preset": "Bush 2", "variants": 1, "height_m": 1.3, "radius_m": 0.8,
        "weight": 1.0, "depth": [0.0, 1.0, null, null] }
    ]
  })JSON";

  ForestCatalog fc;
  REQUIRE(LoadFromText(MinimalForest(body), fc));
  REQUIRE(fc.models.size() == 5);

  float bush1 = 0.0f, bush2 = 0.0f;
  for (size_t i = 0; i < fc.models.size(); ++i) {
    if (fc.models[i].debug_name.rfind("Bush 1", 0) == 0)
      bush1 += fc.type.models[i].weight;
    if (fc.models[i].debug_name.rfind("Bush 2", 0) == 0)
      bush2 += fc.type.models[i].weight;
  }
  CHECK(bush1 == Catch::Approx(bush2));
  CHECK(bush1 == Catch::Approx(1.0f));
}

TEST_CASE("The shipped bush layer has the species mix its file states",
          "[foliage]") {
  // The concrete case the variant-weight bug corrupted. Reads the intended
  // shares straight off the layer, so retuning the file moves both sides
  // together and this stays a statement about the LOADER, not about content.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));
  REQUIRE(fc.type.layers.size() == 3);
  const foliage::FoliageLayer& bush = fc.type.layers[2];

  std::map<std::string, float> share;
  float total = 0.0f;
  for (uint16_t k = 0; k < bush.model_count; ++k) {
    const uint16_t i = static_cast<uint16_t>(bush.first_model + k);
    const std::string species =
        fc.models[i].debug_name.substr(0, fc.models[i].debug_name.rfind(" v"));
    share[species] += fc.type.models[i].weight;
    total += fc.type.models[i].weight;
  }

  // Bush 1 at weight 1.00 against Bush 2's 0.80 and Bush 3's 0.60 is 1/2.4.
  REQUIRE(total == Catch::Approx(2.4f));
  CHECK(share["Bush 1"] / total == Catch::Approx(1.00f / 2.4f));
  CHECK(share["Bush 2"] / total == Catch::Approx(0.80f / 2.4f));
  CHECK(share["Bush 3"] / total == Catch::Approx(0.60f / 2.4f));
}

TEST_CASE("Something is plantable at every depth inside the forest",
          "[foliage]") {
  // A depth band where every model's curve has fallen to zero is a hole in the
  // forest that no amount of density tuning can fill -- and it is invisible in
  // the file, because it is a property of the curves TOGETHER.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  const foliage::FoliageLayer& canopy = fc.type.layers[0];
  for (float d : {1.0f, 5.0f, 12.0f, 25.0f, 60.0f, 1000.0f}) {
    float total = 0.0f;
    for (uint16_t k = 0; k < canopy.model_count; ++k) {
      const foliage::FoliageModel& m = fc.type.models[canopy.first_model + k];
      total += m.weight * m.depth.Evaluate(d);
    }
    INFO("canopy depth " << d);
    CHECK(total > 0.0f);
  }
}

// ------------------------------------------------------------- the loader

TEST_CASE("A missing or unparseable file fails loudly", "[foliage]") {
  ForestCatalog fc;
  CHECK_FALSE(LoadForestCatalog("assets/foliage/does_not_exist.json", fc));
  CHECK(fc.empty());

  CHECK_FALSE(LoadFromText("{ this is not json", fc));
  CHECK(fc.empty());

  CHECK_FALSE(LoadFromText("{}", fc));  // valid JSON, no forest in it
  CHECK(fc.empty());
}

TEST_CASE("A well-formed minimal forest loads", "[foliage]") {
  // The control for every broken case below: without it, a test asserting
  // "this fails" proves nothing, since the base might be what is broken.
  ForestCatalog fc;
  REQUIRE(LoadFromText(MinimalForest(kGoodLayer), fc));
  CHECK(fc.models.size() == 2);  // one species, 2 variants
  CHECK(fc.type.layers.size() == 1);
}

TEST_CASE("An unknown preset name fails rather than planting something else",
          "[foliage]") {
  std::string body = kGoodLayer;
  const size_t at = body.find("Oak (large)");
  REQUIRE(at != std::string::npos);
  body.replace(at, std::string("Oak (large)").size(), "Oka (large)");

  ForestCatalog fc;
  CHECK_FALSE(LoadFromText(MinimalForest(body), fc));
  CHECK(fc.empty());
}

TEST_CASE("A descending depth curve is rejected", "[foliage]") {
  // An inside-out trapezoid does not crash -- it silently evaluates to
  // something nobody intended, which is exactly the kind of bug a screenshot
  // cannot explain.
  std::string body = kGoodLayer;
  const size_t at = body.find("[2.0, 10.0, null, null]");
  REQUIRE(at != std::string::npos);
  body.replace(at, std::string("[2.0, 10.0, null, null]").size(),
               "[10.0, 2.0, null, null]");

  ForestCatalog fc;
  CHECK_FALSE(LoadFromText(MinimalForest(body), fc));
}

TEST_CASE("Out-of-range and missing numbers are rejected", "[foliage]") {
  ForestCatalog fc;

  auto with_replacement = [&](const char* from, const char* to) {
    std::string body = kGoodLayer;
    const size_t at = body.find(from);
    REQUIRE(at != std::string::npos);
    body.replace(at, std::string(from).size(), to);
    return MinimalForest(body);
  };

  CHECK_FALSE(LoadFromText(with_replacement("\"grid_m\": 6.0", "\"grid_m\": 0.0"), fc));
  CHECK_FALSE(LoadFromText(with_replacement("\"grid_m\": 6.0", "\"grid_m\": -3.0"), fc));
  CHECK_FALSE(LoadFromText(with_replacement("\"variants\": 2", "\"variants\": 0"), fc));
  CHECK_FALSE(LoadFromText(
      with_replacement("\"max_slope_deg\": 32.0", "\"max_slope_deg\": 120.0"), fc));
  // A footprint wider than the tree is tall spaces the forest out to nothing.
  CHECK_FALSE(LoadFromText(
      with_replacement("\"radius_m\": 3.4", "\"radius_m\": 30.0"), fc));
  // A missing key, not just a bad value.
  CHECK_FALSE(LoadFromText(with_replacement("\"weight\": 1.0,", ""), fc));
}

TEST_CASE("An inverted clump window is rejected", "[foliage]") {
  // clump_hi below clump_lo inverts the remap, so dense noise reads as glade
  // and the forest comes out photo-negative.
  std::string text = MinimalForest(kGoodLayer);
  const size_t at = text.find("\"clump_hi\": 0.55");
  REQUIRE(at != std::string::npos);
  text.replace(at, std::string("\"clump_hi\": 0.55").size(),
               "\"clump_hi\": 0.10");

  ForestCatalog fc;
  CHECK_FALSE(LoadFromText(text, fc));
}

TEST_CASE("An empty species list is rejected", "[foliage]") {
  std::string body = kGoodLayer;
  const size_t at = body.find("\"species\": [");
  REQUIRE(at != std::string::npos);
  body = body.substr(0, at) + "\"species\": [] }";

  ForestCatalog fc;
  CHECK_FALSE(LoadFromText(MinimalForest(body), fc));
}
