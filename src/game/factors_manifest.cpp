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

// Weight tables are keyed by NAME, not by index: "Hunter" / "Explore" rather
// than 1 / 12. The ids are an append-only wire format the manifest should not
// have to know about, and a named key is what makes a hand-edited tuning file
// reviewable.
int32_t ActivityByName(const std::string& name) {
    for (const ActivityInfo& a : ActivityCatalog()) {
        if (name == a.name) {
            return static_cast<int32_t>(a.id);
        }
    }
    return -1;
}

int32_t HeroClassByName(const std::string& name) {
    for (int32_t c = 0; c < HERO_CLASS_COUNT; ++c) {
        if (name == HeroClassName(static_cast<HeroClassId>(c))) {
            return c;
        }
    }
    return -1;
}

// Overlays { "<Activity>": <number>, ... } onto one weight table. Absent
// activities keep their compiled default; an unknown name is an ERROR, since it
// is otherwise indistinguishable from a weight that silently does nothing.
bool ReadWeights(const nlohmann::json& table, const std::string& where, const std::string& path,
                 ActivityWeights& out) {
    if (!table.is_object()) {
        spdlog::error("LoadSimFactors: '{}' -> {} is not an object", path, where);
        return false;
    }
    for (const auto& [key, value] : table.items()) {
        const int32_t id = ActivityByName(key);
        if (id < 0) {
            spdlog::error("LoadSimFactors: '{}' -> {}.{} is not a known activity", path, where,
                          key);
            return false;
        }
        if (!value.is_number()) {
            spdlog::error("LoadSimFactors: '{}' -> {}.{} is not a number", path, where, key);
            return false;
        }
        out.w[id] = value.get<float>();
    }
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
        // "weights": { "<Hero class>": { "<Activity>": <number>, ... }, ... }
        // The per-class personality dial. Every level is optional.
        if (s->contains("weights")) {
            const nlohmann::json& per_class = (*s)["weights"];
            if (!per_class.is_object()) {
                spdlog::error("LoadSimFactors: '{}' -> hero.weights is not an object",
                              manifest_path);
                return false;
            }
            for (const auto& [class_name, table] : per_class.items()) {
                const int32_t cls = HeroClassByName(class_name);
                if (cls < 0) {
                    spdlog::error("LoadSimFactors: '{}' -> hero.weights.{} is not a known hero "
                                  "class",
                                  manifest_path, class_name);
                    return false;
                }
                if (!ReadWeights(table, "hero.weights." + class_name, manifest_path,
                                 h.weights[cls])) {
                    return false;
                }
            }
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
        // Deer have a single weight table (one "class"), same schema minus the
        // per-class level: { "<Activity>": <number>, ... }.
        if (s->contains("weights") &&
            !ReadWeights((*s)["weights"], "critter.weights", manifest_path, c.weights)) {
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
