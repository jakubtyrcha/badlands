#include "game/factors_manifest.hpp"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Reads one optional float. Absent -> leave the default. Present-but-not-a-
// number -> loud failure (a typo must not silently do nothing).
bool ReadFloat(const nlohmann::json& section, const char* section_name, const char* key,
               const std::string& path, float& out) {
    if (!section.contains(key)) {
        return true;  // keep the compiled default
    }
    if (!section[key].is_number()) {
        spdlog::error("LoadSimFactors: '{}' -> {}.{} is not a number", path, section_name,
                      key);
        return false;
    }
    out = section[key].get<float>();
    return true;
}

}  // namespace

bool LoadSimFactors(const std::string& manifest_path, SimFactors& out) {
    std::ifstream file(manifest_path);
    if (!file) {
        spdlog::error("LoadSimFactors: missing factors manifest '{}'", manifest_path);
        return false;
    }
    nlohmann::json manifest;
    try {
        file >> manifest;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("LoadSimFactors: unparseable factors manifest '{}': {}", manifest_path,
                      e.what());
        return false;
    }
    if (!manifest.is_object()) {
        spdlog::error("LoadSimFactors: '{}' is not a JSON object", manifest_path);
        return false;
    }

    // Parse into a scratch copy so a failure half-way leaves `out` untouched.
    SimFactors parsed = out;

    if (manifest.contains("hero")) {
        const nlohmann::json& hero = manifest["hero"];
        if (!hero.is_object()) {
            spdlog::error("LoadSimFactors: '{}' -> \"hero\" is not an object", manifest_path);
            return false;
        }
        if (!ReadFloat(hero, "hero", "fatigue_go_home", manifest_path,
                       parsed.hero.fatigue_go_home) ||
            !ReadFloat(hero, "hero", "fatigue_night", manifest_path,
                       parsed.hero.fatigue_night) ||
            !ReadFloat(hero, "hero", "boredom_tavern", manifest_path,
                       parsed.hero.boredom_tavern) ||
            !ReadFloat(hero, "hero", "roam_radius", manifest_path, parsed.hero.roam_radius)) {
            return false;
        }
    }

    out = parsed;
    return true;
}

}  // namespace badlands
