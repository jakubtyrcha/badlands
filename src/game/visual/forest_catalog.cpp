#include "game/visual/forest_catalog.hpp"

#include <array>
#include <span>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "foliage/hash.hpp"
#include "game/geometry/tree_generator.hpp"  // TreeCatalog

namespace badlands {

namespace {

constexpr float kInf = 3.4e38f;

// One species as it appears in ONE layer. `variants` distinct mesh variants are
// generated from it, each the same preset re-seeded (see VariantSeed).
struct SpeciesEntry {
  const char* preset;  // must match a TreeCatalog() name exactly
  int variants;
  float target_height_m;
  float radius_m;  // exclusion footprint; conifers are narrower than broadleaves
  float weight;    // relative share of its layer
  foliage::DepthCurve depth;  // where in the forest this species belongs
};

// ---------------------------------------------------------------- the content

// Canopy. Depth curves give the species mix a real gradient, and it is the
// ecologically right one: aspen is a PIONEER species that colonizes edges and
// clearings, pine holds the sheltered interior, oak and ash are broad
// generalists sitting across both.
constexpr std::array<SpeciesEntry, 4> kCanopy{{
    {"Oak (large)", 4, 22.0f, 3.4f, 1.00f, {2.0f, 10.0f, kInf, kInf}},
    {"Pine (large)", 4, 27.0f, 2.8f, 1.00f, {6.0f, 20.0f, kInf, kInf}},
    {"Ash (large)", 4, 24.0f, 3.2f, 0.80f, {2.0f, 10.0f, kInf, kInf}},
    {"Aspen (large)", 4, 19.0f, 2.4f, 0.70f, {0.0f, 4.0f, 18.0f, 30.0f}},
}};

// Saplings. Same pioneer/interior split, compressed into the narrower band the
// sapling layer occupies.
constexpr std::array<SpeciesEntry, 4> kSapling{{
    {"Oak (small)", 2, 5.0f, 1.5f, 1.00f, {0.0f, 2.0f, 20.0f, 40.0f}},
    {"Pine (small)", 2, 5.5f, 1.2f, 0.90f, {4.0f, 14.0f, kInf, kInf}},
    {"Ash (small)", 2, 4.5f, 1.4f, 0.80f, {0.0f, 2.0f, 20.0f, 40.0f}},
    {"Aspen (small)", 2, 4.0f, 1.1f, 1.10f, {0.0f, 1.0f, 12.0f, 26.0f}},
}};

// Shrub layer. Bush 1 gets two variants because it is the one that carries the
// forest edge; Bush 3 (the evergreen) leans interior.
constexpr std::array<SpeciesEntry, 3> kBush{{
    {"Bush 1", 2, 1.5f, 0.90f, 1.00f, {0.0f, 1.0f, 14.0f, 26.0f}},
    {"Bush 2", 1, 1.3f, 0.80f, 0.80f, {0.0f, 1.0f, 12.0f, 24.0f}},
    {"Bush 3", 1, 1.1f, 0.70f, 0.60f, {3.0f, 10.0f, kInf, kInf}},
}};

struct LayerEntry {
  std::span<const SpeciesEntry> species;
  float grid_m;
  float max_slope_deg;
  foliage::DepthCurve density;
  glm::vec2 scale_range;
  float edge_scale;
  float edge_scale_depth_m;
};

// Layer order IS placement priority: canopy first, so the big trees claim their
// space before saplings and bushes fill the gaps between them.
//
// grid_m is the density ceiling (one candidate per square). The canopy's 5 m
// against a 3.4 m radius targets a mature closed stand at roughly 40-60 m^2 per
// stem once the clump field and depth curve have taken their share -- the real
// figure for temperate forest.
const std::array<LayerEntry, 3> kLayers{{
    {kCanopy, 5.0f, 32.0f, {1.0f, 14.0f, kInf, kInf}, {0.85f, 1.15f}, 0.55f, 25.0f},
    {kSapling, 3.0f, 36.0f, {0.0f, 3.0f, 16.0f, 34.0f}, {0.75f, 1.25f}, 0.80f, 12.0f},
    {kBush, 1.6f, 40.0f, {0.0f, 1.0f, 10.0f, 22.0f}, {0.80f, 1.30f}, 1.0f, 1.0f},
}};

// ------------------------------------------------------------------ machinery

// Variant 0 keeps the preset's ORIGINAL seed, so it is byte-identical to the
// tree the model viewer shows for that catalog entry -- a variant list that
// silently excluded the canonical tree would make the viewer a liar. Later
// variants take a well-mixed re-seed.
uint32_t VariantSeed(uint32_t preset_seed, int variant) {
  if (variant == 0) return preset_seed;
  return foliage::Triple32(preset_seed ^
                           (static_cast<uint32_t>(variant) * 0x9e3779b9u));
}

bool AppendLayer(const LayerEntry& layer,
                 const std::unordered_map<std::string, const TreeOptions*>& by_name,
                 ForestCatalog& out) {
  foliage::FoliageLayer fl;
  fl.grid_m = layer.grid_m;
  fl.max_slope_deg = layer.max_slope_deg;
  fl.density = layer.density;
  fl.edge_scale = layer.edge_scale;
  fl.edge_scale_depth_m = layer.edge_scale_depth_m;
  fl.first_model = static_cast<uint16_t>(out.type.models.size());

  for (const SpeciesEntry& s : layer.species) {
    const auto it = by_name.find(s.preset);
    if (it == by_name.end()) {
      spdlog::error(
          "BuildForestCatalog: TreeCatalog() has no preset named '{}' -- the "
          "catalog was reordered or renamed; refusing to plant the wrong tree",
          s.preset);
      return false;
    }

    for (int v = 0; v < s.variants; ++v) {
      ForestModelSpec spec;
      spec.options = *it->second;
      spec.options.seed = VariantSeed(it->second->seed, v);
      spec.target_height_m = s.target_height_m;
      spec.debug_name = std::string(s.preset) + " v" + std::to_string(v);
      out.models.push_back(std::move(spec));

      foliage::FoliageModel fm;
      fm.radius_m = s.radius_m;
      fm.height_m = s.target_height_m;
      fm.scale_range = layer.scale_range;
      // Every species in a layer has the same variant count, so giving each
      // variant the full species weight preserves the intended species ratio
      // without dividing it out.
      fm.weight = s.weight;
      fm.depth = s.depth;
      out.type.models.push_back(fm);
    }
  }

  fl.model_count =
      static_cast<uint16_t>(out.type.models.size() - fl.first_model);
  out.type.layers.push_back(fl);
  return true;
}

}  // namespace

bool BuildForestCatalog(ForestCatalog& out) {
  out = {};

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  std::unordered_map<std::string, const TreeOptions*> by_name;
  by_name.reserve(catalog.size());
  for (const NamedTreeOptions& n : catalog) by_name[n.name] = &n.options;

  for (const LayerEntry& layer : kLayers) {
    if (!AppendLayer(layer, by_name, out)) {
      out = {};
      return false;
    }
  }

  if (!out.type.Valid()) {
    spdlog::error("BuildForestCatalog: assembled a malformed ForestType");
    out = {};
    return false;
  }
  return true;
}

}  // namespace badlands
