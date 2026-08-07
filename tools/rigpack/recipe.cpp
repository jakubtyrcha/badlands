#include "rigpack/recipe.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace badlands::rigpack {

std::optional<Recipe> LoadRecipe(const std::filesystem::path& path,
                                 std::string* error) {
  std::ifstream stream(path);
  if (!stream) {
    *error = "cannot open " + path.string();
    return std::nullopt;
  }

  // ordered_json: the recipe's clip order becomes the manifest's clip order,
  // and so the order a viewer lists them in. A std::map-backed parse would
  // alphabetize it and "attack" would silently become clip 0.
  nlohmann::ordered_json doc;
  try {
    stream >> doc;
  } catch (const nlohmann::json::exception& e) {
    *error = path.string() + " is not valid JSON: " + e.what();
    return std::nullopt;
  }

  // Everything below reads fields out of a hand-authored file, and nlohmann's
  // value() THROWS when a key exists with the wrong type -- `"yaw_offset_degrees":
  // "0"` would abort the process. The tool promises every failure comes back as
  // a report, so the whole body is guarded rather than each field.
  try {
  Recipe recipe;
  recipe.out_dir = path.parent_path();
  recipe.family = doc.value("family", std::string());

  if (const auto it = doc.find("families"); it != doc.end() && it->is_array()) {
    for (const auto& value : *it) {
      if (!value.is_string()) {
        *error = path.string() + " has a \"families\" entry that is not a string";
        return std::nullopt;
      }
      recipe.families.push_back(value.get<std::string>());
    }
  }

  if (recipe.family.empty() == recipe.families.empty()) {
    *error = path.string() +
             " must name exactly one of \"family\" or \"families\"";
    return std::nullopt;
  }
  recipe.yaw_offset_degrees = doc.value("yaw_offset_degrees", 0.0f);

  const auto clips_it = doc.find("clips");
  if (clips_it == doc.end()) {
    *error = path.string() + " has no \"clips\"";
    return std::nullopt;
  }

  // "*" means the whole family, each clip keeping its own name. The expansion
  // happens in the packer, which is the only place that knows what the family
  // actually contains.
  if (clips_it->is_string()) {
    if (clips_it->get<std::string>() != "*") {
      *error = path.string() + " has a \"clips\" string that is not \"*\"";
      return std::nullopt;
    }
    recipe.all_clips = true;
    return recipe;
  }

  if (!clips_it->is_object() || clips_it->empty()) {
    *error = path.string() + " has no non-empty \"clips\" object";
    return std::nullopt;
  }
  for (const auto& [logical, source] : clips_it->items()) {
    if (!logical.empty() && logical.front() == '_') continue;  // comment key
    if (!source.is_string()) {
      *error = "clip \"" + logical + "\" does not name an intermediate clip";
      return std::nullopt;
    }
    recipe.clips.emplace_back(logical, source.get<std::string>());
  }
  if (recipe.clips.empty()) {
    *error = path.string() + " names no clips";
    return std::nullopt;
  }

  return recipe;
  } catch (const nlohmann::json::exception& e) {
    *error = path.string() + " has a field of the wrong type: " + e.what();
    return std::nullopt;
  }
}

}  // namespace badlands::rigpack
