#include "game/factors_manifest.hpp"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Reads one optional numeric key into `out` (any arithmetic type). Absent ->
// keep the default. Present-but-not-a-number -> loud failure (a typo must not
// silently do nothing).
template <typename T>
bool ReadNum(const nlohmann::json& section, const char* section_name, const char* key,
             const std::string& path, T& out) {
    if (!section.contains(key)) {
        return true;  // keep the compiled default
    }
    if (!section[key].is_number()) {
        spdlog::error("LoadSimFactors: '{}' -> {}.{} is not a number", path, section_name, key);
        return false;
    }
    out = section[key].get<T>();
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

    // Each section is optional; a present section must be an object.
    auto section = [&](const char* name, const nlohmann::json*& out_obj) -> bool {
        if (!manifest.contains(name)) {
            out_obj = nullptr;
            return true;
        }
        if (!manifest[name].is_object()) {
            spdlog::error("LoadSimFactors: '{}' -> \"{}\" is not an object", manifest_path,
                          name);
            return false;
        }
        out_obj = &manifest[name];
        return true;
    };

    const nlohmann::json* s = nullptr;
    if (!section("hero", s)) {
        return false;
    }
    if (s != nullptr) {
        HeroFactors& h = parsed.hero;
        if (!ReadNum(*s, "hero", "fatigue_go_home", manifest_path, h.fatigue_go_home) ||
            !ReadNum(*s, "hero", "fatigue_night", manifest_path, h.fatigue_night) ||
            !ReadNum(*s, "hero", "boredom_tavern", manifest_path, h.boredom_tavern) ||
            !ReadNum(*s, "hero", "roam_radius", manifest_path, h.roam_radius) ||
            !ReadNum(*s, "hero", "hunt_sight_radius", manifest_path, h.hunt_sight_radius)) {
            return false;
        }
    }
    if (!section("critter", s)) {
        return false;
    }
    if (s != nullptr) {
        CritterFactors& c = parsed.critter;
        if (!ReadNum(*s, "critter", "sight_radius", manifest_path, c.sight_radius) ||
            !ReadNum(*s, "critter", "flee_radius", manifest_path, c.flee_radius) ||
            !ReadNum(*s, "critter", "flee_distance", manifest_path, c.flee_distance) ||
            !ReadNum(*s, "critter", "roam_radius", manifest_path, c.roam_radius) ||
            !ReadNum(*s, "critter", "graze_fraction", manifest_path, c.graze_fraction)) {
            return false;
        }
    }
    if (!section("townfolk", s)) {
        return false;
    }
    if (s != nullptr) {
        TownfolkFactors& t = parsed.townfolk;
        if (!ReadNum(*s, "townfolk", "spawn_interval_millis", manifest_path,
                     t.spawn_interval_millis) ||
            !ReadNum(*s, "townfolk", "max_alive", manifest_path, t.max_alive) ||
            !ReadNum(*s, "townfolk", "move_speed", manifest_path, t.move_speed) ||
            !ReadNum(*s, "townfolk", "house_income_per_day", manifest_path,
                     t.house_income_per_day)) {
            return false;
        }
    }
    if (!section("monster", s)) {
        return false;
    }
    if (s != nullptr) {
        MonsterFactors& m = parsed.monster;
        if (!ReadNum(*s, "monster", "spawn_interval_millis", manifest_path,
                     m.spawn_interval_millis) ||
            !ReadNum(*s, "monster", "max_alive", manifest_path, m.max_alive)) {
            return false;
        }
    }

    out = parsed;
    return true;
}

}  // namespace badlands
