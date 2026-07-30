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
        if (!ReadNum(*s, "hero", "fatigue_drain_hours", manifest_path, h.fatigue_drain_hours) ||
            !ReadNum(*s, "hero", "content_drain_hours", manifest_path, h.content_drain_hours) ||
            !ReadNum(*s, "hero", "rest_fill_hours", manifest_path, h.rest_fill_hours) ||
            !ReadNum(*s, "hero", "tavern_fill_hours", manifest_path, h.tavern_fill_hours) ||
            !ReadNum(*s, "hero", "fatigue_seek", manifest_path, h.fatigue_seek) ||
            !ReadNum(*s, "hero", "fatigue_seek_night", manifest_path, h.fatigue_seek_night) ||
            !ReadNum(*s, "hero", "content_seek", manifest_path, h.content_seek) ||
            !ReadNum(*s, "hero", "low_health_rest", manifest_path, h.low_health_rest) ||
            !ReadNum(*s, "hero", "roam_radius", manifest_path, h.roam_radius) ||
            !ReadNum(*s, "hero", "hunt_sight_radius", manifest_path, h.hunt_sight_radius) ||
            !ReadNum(*s, "hero", "threat_radius", manifest_path, h.threat_radius) ||
            !ReadNum(*s, "hero", "memory_ttl_millis", manifest_path, h.memory_ttl_millis) ||
            !ReadNum(*s, "hero", "think_min_millis", manifest_path, h.think_min_millis) ||
            !ReadNum(*s, "hero", "think_max_millis", manifest_path, h.think_max_millis) ||
            !ReadNum(*s, "hero", "chat_content_seek", manifest_path, h.chat_content_seek) ||
            !ReadNum(*s, "hero", "chat_fill_hours", manifest_path, h.chat_fill_hours) ||
            !ReadNum(*s, "hero", "chat_content_ceiling", manifest_path,
                     h.chat_content_ceiling) ||
            !ReadNum(*s, "hero", "chat_sight", manifest_path, h.chat_sight) ||
            !ReadNum(*s, "hero", "chat_radius", manifest_path, h.chat_radius) ||
            !ReadNum(*s, "hero", "chat_duration", manifest_path, h.chat_duration) ||
            !ReadNum(*s, "hero", "explore_min_fatigue", manifest_path, h.explore_min_fatigue) ||
            !ReadNum(*s, "hero", "explore_min_distance", manifest_path, h.explore_min_distance) ||
            !ReadNum(*s, "hero", "explore_max_distance", manifest_path, h.explore_max_distance) ||
            !ReadNum(*s, "hero", "explore_search_radius", manifest_path,
                     h.explore_search_radius) ||
            !ReadNum(*s, "hero", "explore_lease_millis", manifest_path, h.explore_lease_millis)) {
            return false;
        }
        // "explore_chance": { "<Hero class>": <0..1>, ... } -- how often a class
        // feels like exploring at all. Keyed by name, same policy as weights.
        if (s->contains("explore_chance")) {
            const nlohmann::json& per_class = (*s)["explore_chance"];
            if (!per_class.is_object()) {
                spdlog::error("LoadSimFactors: '{}' -> hero.explore_chance is not an object",
                              manifest_path);
                return false;
            }
            for (const auto& [class_name, value] : per_class.items()) {
                const int32_t cls = HeroClassByName(class_name);
                if (cls < 0) {
                    spdlog::error("LoadSimFactors: '{}' -> hero.explore_chance.{} is not a known "
                                  "hero class",
                                  manifest_path, class_name);
                    return false;
                }
                if (!value.is_number()) {
                    spdlog::error("LoadSimFactors: '{}' -> hero.explore_chance.{} is not a number",
                                  manifest_path, class_name);
                    return false;
                }
                h.explore_chance[cls] = value.get<float>();
            }
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
    if (!section("progression", s)) {
        return false;
    }
    if (s != nullptr) {
        ProgressionFactors& p = parsed.progression;
        if (!ReadNum(*s, "progression", "xp_per_texel", manifest_path, p.xp_per_texel) ||
            !ReadNum(*s, "progression", "kill_xp_radius", manifest_path, p.kill_xp_radius) ||
            !ReadNum(*s, "progression", "level_base_xp", manifest_path, p.level_base_xp) ||
            !ReadNum(*s, "progression", "level_exponent", manifest_path, p.level_exponent)) {
            return false;
        }
    }

    out = parsed;
    return true;
}

}  // namespace badlands
