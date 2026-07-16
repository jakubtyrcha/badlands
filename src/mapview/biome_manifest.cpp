#include "mapview/biome_manifest.hpp"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "mapgen/biomes.hpp"

namespace badlands {

bool ResolveBiomePacks(const std::string& manifest_path,
                       std::vector<std::string>& out_pack_dirs) {
  out_pack_dirs.clear();

  std::ifstream file(manifest_path);
  if (!file) {
    spdlog::error("ResolveBiomePacks: missing biome manifest '{}'",
                  manifest_path);
    return false;
  }
  nlohmann::json manifest;
  try {
    file >> manifest;
  } catch (const nlohmann::json::exception& e) {
    spdlog::error("ResolveBiomePacks: unparseable biome manifest '{}': {}",
                  manifest_path, e.what());
    return false;
  }

  // Layer index == Biome enum value, so resolve in enum order.
  out_pack_dirs.reserve(mapgen::kBiomeCount);
  for (int i = 0; i < mapgen::kBiomeCount; ++i) {
    const std::string name(mapgen::biome_name(static_cast<mapgen::Biome>(i)));
    if (!manifest.contains(name) || !manifest[name].is_string()) {
      spdlog::error(
          "ResolveBiomePacks: manifest '{}' has no pack string for biome '{}'",
          manifest_path, name);
      out_pack_dirs.clear();
      return false;
    }
    out_pack_dirs.push_back(manifest[name].get<std::string>());
  }
  return true;
}

}  // namespace badlands
