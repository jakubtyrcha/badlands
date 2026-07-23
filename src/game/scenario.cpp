#include "game/scenario.h"

#include <fstream>
#include <map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

ArenaTally tally_arena(const std::vector<CharacterState>& rows) {
    std::map<int32_t, int> alive;  // team -> living combatants (ascending team order)
    for (const CharacterState& r : rows) {
        if (r.archetype == static_cast<int32_t>(Archetype::Critter)) {
            continue;  // neutral wildlife -- never a combatant, whatever its team
        }
        ++alive[r.team];
    }
    ArenaTally t;
    for (const auto& [team, count] : alive) {
        t.teams.emplace_back(team, count);
    }
    t.winner = (t.teams.size() == 1) ? t.teams.front().first : -1;
    return t;
}

bool LoadScenario(const std::string& path, Scenario& out) {
    std::ifstream file(path);
    if (!file.good()) {
        spdlog::warn("LoadScenario: cannot open '{}'", path);
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("LoadScenario: '{}' is not valid JSON: {}", path, e.what());
        return false;
    }
    if (!j.is_object()) {
        spdlog::warn("LoadScenario: '{}' top level is not an object", path);
        return false;
    }

    Scenario parsed;
    parsed.name = j.value("name", std::string("scenario"));

    if (j.contains("arena")) {
        const nlohmann::json& a = j["arena"];
        if (!a.is_object() || !a.contains("half_x") || !a.contains("half_z") ||
            !a["half_x"].is_number() || !a["half_z"].is_number()) {
            spdlog::warn("LoadScenario: '{}' arena needs numeric half_x/half_z", path);
            return false;
        }
        parsed.arena_half_x = a["half_x"].get<float>();
        parsed.arena_half_z = a["half_z"].get<float>();
    }

    if (j.contains("spawns")) {
        const nlohmann::json& spawns = j["spawns"];
        if (!spawns.is_array()) {
            spdlog::warn("LoadScenario: '{}' spawns must be an array", path);
            return false;
        }
        for (const nlohmann::json& s : spawns) {
            if (!s.is_object() || !s.contains("creature") || !s["creature"].is_string()) {
                spdlog::warn("LoadScenario: '{}' each spawn needs a creature name", path);
                return false;
            }
            const std::string name = s["creature"].get<std::string>();
            const CreatureId id = CreatureIdFromName(name.c_str());
            if (id == CreatureId::Count) {
                spdlog::warn("LoadScenario: '{}' unknown creature '{}'", path, name);
                return false;
            }
            ScenarioSpawn spawn;
            spawn.creature = id;
            spawn.team = s.value("team", 0);
            spawn.x = s.value("x", 0.0f);
            spawn.z = s.value("z", 0.0f);
            parsed.spawns.push_back(spawn);
        }
    }

    out = std::move(parsed);
    return true;
}

}  // namespace badlands
