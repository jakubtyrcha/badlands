// badlands' forest content table: that it resolves against TreeCatalog(), that
// its layer slices are well formed, and — the one that actually matters — that
// a "variant" is a genuinely different mesh rather than the same tree listed
// four times.

#include <catch_amalgamated.hpp>

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

}  // namespace

TEST_CASE("BuildForestCatalog resolves every preset it names", "[foliage]") {
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));
  CHECK(fc.type.Valid());
  CHECK(fc.models.size() == fc.type.models.size());

  // 16 canopy + 8 sapling + 4 bush.
  CHECK(fc.models.size() == 28);
  CHECK(fc.type.layers.size() == 3);
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

  CHECK(fc.type.layers[0].model_count == 16);  // canopy
  CHECK(fc.type.layers[1].model_count == 8);   // sapling
  CHECK(fc.type.layers[2].model_count == 4);   // bush
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
  CHECK(checked == 11);  // one v0 per species entry: 4 canopy + 4 sapling + 3 bush
}

TEST_CASE("Variants of one species are genuinely different meshes", "[foliage]") {
  // A variant is only worth its GPU slot if the skeleton actually differs.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  // Group by preset name (debug_name minus the " vN" suffix).
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

  // Spot-check that a re-seed really moves the geometry: same preset, two
  // variants, different skeletons.
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

TEST_CASE("Model heights and radii are physically ordered", "[foliage]") {
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));

  const foliage::FoliageLayer& canopy = fc.type.layers[0];
  const foliage::FoliageLayer& sapling = fc.type.layers[1];
  const foliage::FoliageLayer& bush = fc.type.layers[2];

  auto min_height = [&](const foliage::FoliageLayer& l) {
    float m = 1e9f;
    for (uint16_t k = 0; k < l.model_count; ++k)
      m = std::min(m, fc.type.models[l.first_model + k].height_m);
    return m;
  };
  auto max_height = [&](const foliage::FoliageLayer& l) {
    float m = 0.0f;
    for (uint16_t k = 0; k < l.model_count; ++k)
      m = std::max(m, fc.type.models[l.first_model + k].height_m);
    return m;
  };

  // Layers must not overlap in stature, or "sapling" and "canopy" stop meaning
  // anything on screen.
  CHECK(max_height(bush) < min_height(sapling));
  CHECK(max_height(sapling) < min_height(canopy));

  // Real trees: canopy in the 18-30 m band, saplings a few metres, bushes
  // knee-to-chest.
  CHECK(min_height(canopy) >= 18.0f);
  CHECK(max_height(canopy) <= 30.0f);
  CHECK(max_height(bush) <= 2.0f);

  // Every model has a usable footprint, and radius scales with stature.
  for (const foliage::FoliageModel& m : fc.type.models) {
    CHECK(m.radius_m > 0.0f);
    CHECK(m.radius_m < m.height_m);
    CHECK(m.scale_range.x > 0.0f);
    CHECK(m.scale_range.y >= m.scale_range.x);
    CHECK(m.weight > 0.0f);
  }
}

TEST_CASE("Aspen leads the edge and pine holds the interior", "[foliage]") {
  // The ecological gradient the canopy's per-model depth curves encode: aspen
  // is a pioneer, pine is an interior conifer. If these ever invert, the
  // species mix reads backwards from edge to core.
  ForestCatalog fc;
  REQUIRE(BuildForestCatalog(fc));
  const foliage::FoliageLayer& canopy = fc.type.layers[0];

  auto weight_of = [&](const std::string& species, float depth) {
    float w = 0.0f;
    for (uint16_t k = 0; k < canopy.model_count; ++k) {
      const uint16_t i = static_cast<uint16_t>(canopy.first_model + k);
      if (fc.models[i].debug_name.rfind(species, 0) != 0) continue;
      w += fc.type.models[i].weight * fc.type.models[i].depth.Evaluate(depth);
    }
    return w;
  };

  // At the very edge, aspen outweighs pine.
  CHECK(weight_of("Aspen", 3.0f) > weight_of("Pine", 3.0f));
  // Deep inside, pine outweighs aspen (whose curve has fallen to zero).
  CHECK(weight_of("Pine", 40.0f) > weight_of("Aspen", 40.0f));
  CHECK(weight_of("Aspen", 40.0f) == 0.0f);
  // And something is always plantable at any depth inside the forest.
  for (float d : {1.0f, 5.0f, 12.0f, 25.0f, 60.0f, 1000.0f}) {
    float total = 0.0f;
    for (uint16_t k = 0; k < canopy.model_count; ++k) {
      const uint16_t i = static_cast<uint16_t>(canopy.first_model + k);
      total += fc.type.models[i].weight * fc.type.models[i].depth.Evaluate(d);
    }
    INFO("depth " << d);
    CHECK(total > 0.0f);
  }
}
