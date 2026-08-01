#include "game/visual/forest_catalog.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "foliage/hash.hpp"
#include "game/geometry/tree_generator.hpp"  // TreeCatalog

namespace badlands {

namespace {

using nlohmann::json;

constexpr float kInf = std::numeric_limits<float>::max();

// Variant 0 keeps the preset's ORIGINAL seed, so it is byte-identical to the
// tree the model viewer shows for that catalog entry -- a variant list that
// silently excluded the canonical tree would make the viewer a liar. Later
// variants take a well-mixed re-seed.
uint32_t VariantSeed(uint32_t preset_seed, int variant) {
  if (variant == 0) return preset_seed;
  return foliage::Triple32(preset_seed ^
                           (static_cast<uint32_t>(variant) * 0x9e3779b9u));
}

// Every read below reports the exact path it failed at (e.g.
// "layers[1].species[2].radius_m"), because the whole point of loading this
// from a file is that a human is editing it by hand.
bool ReadNumber(const json& obj, const std::string& where, const char* key,
                float& out, float min_value, float max_value) {
  if (!obj.contains(key)) {
    spdlog::error("forest catalog: {}.{} is missing", where, key);
    return false;
  }
  if (!obj[key].is_number()) {
    spdlog::error("forest catalog: {}.{} is not a number", where, key);
    return false;
  }
  const float v = obj[key].get<float>();
  if (!std::isfinite(v) || v < min_value || v > max_value) {
    spdlog::error("forest catalog: {}.{} = {} is out of range [{}, {}]", where,
                  key, v, min_value, max_value);
    return false;
  }
  out = v;
  return true;
}

bool ReadInt(const json& obj, const std::string& where, const char* key,
             int& out, int min_value, int max_value) {
  if (!obj.contains(key) || !obj[key].is_number_integer()) {
    spdlog::error("forest catalog: {}.{} is missing or not an integer", where,
                  key);
    return false;
  }
  const int v = obj[key].get<int>();
  if (v < min_value || v > max_value) {
    spdlog::error("forest catalog: {}.{} = {} is out of range [{}, {}]", where,
                  key, v, min_value, max_value);
    return false;
  }
  out = v;
  return true;
}

// A trapezoid as [rise_start, rise_end, fall_start, fall_end], metres. `null`
// means "never falls" -- JSON has no infinity, and writing a huge sentinel by
// hand in every canopy entry would be worse than a keyword.
bool ReadCurve(const json& obj, const std::string& where, const char* key,
               foliage::DepthCurve& out) {
  if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 4) {
    spdlog::error(
        "forest catalog: {}.{} must be a 4-element array "
        "[rise_start, rise_end, fall_start, fall_end]",
        where, key);
    return false;
  }

  float v[4];
  for (size_t i = 0; i < 4; ++i) {
    const json& e = obj[key][i];
    if (e.is_null()) {
      v[i] = kInf;
      continue;
    }
    if (!e.is_number()) {
      spdlog::error("forest catalog: {}.{}[{}] is neither a number nor null",
                    where, key, i);
      return false;
    }
    v[i] = e.get<float>();
    if (!std::isfinite(v[i]) || v[i] < 0.0f) {
      spdlog::error("forest catalog: {}.{}[{}] = {} must be >= 0", where, key,
                    i, v[i]);
      return false;
    }
  }

  // Non-decreasing, or the trapezoid is inside out -- which does not crash,
  // it just silently evaluates to something nobody intended.
  for (size_t i = 1; i < 4; ++i) {
    if (v[i] < v[i - 1]) {
      spdlog::error(
          "forest catalog: {}.{} is not ascending ({} then {}); a curve must "
          "read [rise_start, rise_end, fall_start, fall_end]",
          where, key, v[i - 1], v[i]);
      return false;
    }
  }

  out = {v[0], v[1], v[2], v[3]};
  return true;
}

bool ReadRange(const json& obj, const std::string& where, const char* key,
               glm::vec2& out) {
  if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 2 ||
      !obj[key][0].is_number() || !obj[key][1].is_number()) {
    spdlog::error("forest catalog: {}.{} must be a 2-element number array",
                  where, key);
    return false;
  }
  const float lo = obj[key][0].get<float>();
  const float hi = obj[key][1].get<float>();
  if (!(lo > 0.0f) || hi < lo) {
    spdlog::error("forest catalog: {}.{} = [{}, {}] must be 0 < lo <= hi",
                  where, key, lo, hi);
    return false;
  }
  out = glm::vec2(lo, hi);
  return true;
}

bool ReadNoise(const json& root, foliage::ForestNoise& out) {
  if (!root.contains("noise")) {
    spdlog::error("forest catalog: missing 'noise' object");
    return false;
  }
  const json& n = root["noise"];
  const std::string where = "noise";

  int octaves = 0;
  if (!ReadNumber(n, where, "clump_wavelength_m", out.clump_wavelength_m, 0.01f,
                  100000.0f) ||
      !ReadInt(n, where, "clump_octaves", octaves, 1, 8) ||
      !ReadNumber(n, where, "clump_lo", out.clump_lo, 0.0f, 1.0f) ||
      !ReadNumber(n, where, "clump_hi", out.clump_hi, 0.0f, 1.0f) ||
      !ReadNumber(n, where, "warp_amp_m", out.warp_amp_m, 0.0f, 1000.0f) ||
      !ReadNumber(n, where, "warp_wavelength_m", out.warp_wavelength_m, 0.01f,
                  100000.0f)) {
    return false;
  }
  out.clump_octaves = octaves;

  if (out.clump_hi < out.clump_lo) {
    spdlog::error(
        "forest catalog: noise.clump_hi {} is below clump_lo {} -- the window "
        "would invert, making dense noise read as glade",
        out.clump_hi, out.clump_lo);
    return false;
  }
  return true;
}

bool ReadLayer(const json& l, const std::string& where,
               const std::unordered_map<std::string, const TreeOptions*>& by_name,
               ForestCatalog& out) {
  foliage::FoliageLayer fl;
  glm::vec2 scale_range{1.0f, 1.0f};

  if (!ReadNumber(l, where, "grid_m", fl.grid_m, 0.05f, 1000.0f) ||
      !ReadNumber(l, where, "max_slope_deg", fl.max_slope_deg, 0.0f, 90.0f) ||
      !ReadCurve(l, where, "density", fl.density) ||
      !ReadRange(l, where, "scale_range", scale_range) ||
      !ReadNumber(l, where, "edge_scale", fl.edge_scale, 0.01f, 1.0f) ||
      !ReadNumber(l, where, "edge_scale_depth_m", fl.edge_scale_depth_m, 0.01f,
                  1000.0f)) {
    return false;
  }

  if (!l.contains("species") || !l["species"].is_array() ||
      l["species"].empty()) {
    spdlog::error("forest catalog: {}.species must be a non-empty array", where);
    return false;
  }

  fl.first_model = static_cast<uint16_t>(out.type.models.size());

  for (size_t si = 0; si < l["species"].size(); ++si) {
    const json& s = l["species"][si];
    const std::string swhere = where + ".species[" + std::to_string(si) + "]";

    if (!s.contains("preset") || !s["preset"].is_string()) {
      spdlog::error("forest catalog: {}.preset is missing or not a string",
                    swhere);
      return false;
    }
    const std::string preset = s["preset"].get<std::string>();
    const auto it = by_name.find(preset);
    if (it == by_name.end()) {
      spdlog::error(
          "forest catalog: {}.preset '{}' is not a TreeCatalog() entry -- the "
          "catalog was reordered or renamed, or the name is misspelled",
          swhere, preset);
      return false;
    }

    int variants = 0;
    foliage::FoliageModel fm;
    fm.scale_range = scale_range;
    if (!ReadInt(s, swhere, "variants", variants, 1, 16) ||
        !ReadNumber(s, swhere, "height_m", fm.height_m, 0.05f, 200.0f) ||
        !ReadNumber(s, swhere, "radius_m", fm.radius_m, 0.01f, 100.0f) ||
        !ReadNumber(s, swhere, "weight", fm.weight, 0.0001f, 1000.0f) ||
        !ReadCurve(s, swhere, "depth", fm.depth)) {
      return false;
    }
    if (fm.radius_m >= fm.height_m) {
      spdlog::error(
          "forest catalog: {} has radius_m {} >= height_m {} -- a footprint "
          "wider than the tree is tall spaces the forest out to nothing",
          swhere, fm.radius_m, fm.height_m);
      return false;
    }

    // `weight` is the SPECIES' share of its layer, so it has to be split
    // across that species' variants: the sampler picks over models, and
    // handing each variant the full weight would make a species' actual
    // share weight * variants. Live in the shipped file before this fix --
    // Bush 1 (weight 1.00, 2 variants) was taking 59% of bushes against the
    // 42% the file reads as. Variants are a mesh-variety concern; they must
    // not move the ecological mix.
    const float per_variant_weight = fm.weight / static_cast<float>(variants);
    for (int v = 0; v < variants; ++v) {
      ForestModelSpec spec;
      spec.options = *it->second;
      spec.options.seed = VariantSeed(it->second->seed, v);
      spec.target_height_m = fm.height_m;
      spec.debug_name = preset + " v" + std::to_string(v);
      out.models.push_back(std::move(spec));

      foliage::FoliageModel variant_model = fm;
      variant_model.weight = per_variant_weight;
      out.type.models.push_back(variant_model);
    }
  }

  fl.model_count =
      static_cast<uint16_t>(out.type.models.size() - fl.first_model);
  out.type.layers.push_back(fl);
  return true;
}

}  // namespace

bool LoadForestCatalog(const std::string& path, ForestCatalog& out) {
  out = {};

  std::ifstream file(path);
  if (!file) {
    spdlog::error("forest catalog: cannot open '{}'", path);
    return false;
  }
  json root;
  try {
    file >> root;
  } catch (const json::exception& e) {
    spdlog::error("forest catalog: '{}' is not valid JSON: {}", path, e.what());
    return false;
  }

  if (!ReadNoise(root, out.type.noise)) {
    out = {};
    return false;
  }

  if (!root.contains("layers") || !root["layers"].is_array() ||
      root["layers"].empty()) {
    spdlog::error("forest catalog: '{}' has no non-empty 'layers' array", path);
    out = {};
    return false;
  }

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  std::unordered_map<std::string, const TreeOptions*> by_name;
  by_name.reserve(catalog.size());
  for (const NamedTreeOptions& n : catalog) by_name[n.name] = &n.options;

  for (size_t li = 0; li < root["layers"].size(); ++li) {
    const std::string where = "layers[" + std::to_string(li) + "]";
    if (!ReadLayer(root["layers"][li], where, by_name, out)) {
      out = {};
      return false;
    }
  }

  if (!out.type.Valid()) {
    spdlog::error("forest catalog: '{}' assembled a malformed ForestType", path);
    out = {};
    return false;
  }

  spdlog::info("forest catalog: '{}' -> {} layers, {} models", path,
               out.type.layers.size(), out.models.size());
  return true;
}

}  // namespace badlands
