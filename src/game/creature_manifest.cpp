#include "game/creature_manifest.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Read an optional numeric key into `dst` (any arithmetic type). Returns false
// only on a present-but-non-numeric value (a typo worth failing on); a missing
// key is fine.
template <typename T>
bool ReadNum(const nlohmann::json& obj, const char* creature, const char* key, T& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number()) {
        spdlog::warn("LoadCreatureCatalog: {}.{} is not a number", creature, key);
        return false;
    }
    dst = obj[key].get<T>();
    return true;
}

// Read the optional "skills" array: [{ "name": "ShieldBash", "level": 3 }, ...],
// the level-gated acquisition list for this creature. REPLACES the compiled
// list wholesale when present (an override that merged would give no way to
// take a skill away), and any malformed entry fails the load -- a typo that
// silently cost a class its skill would be found in play, not in the log.
bool ReadSkillGrants(const nlohmann::json& obj, const char* creature, CharacterDesc& d) {
    if (!obj.contains("skills")) {
        return true;
    }
    if (!obj["skills"].is_array()) {
        spdlog::warn("LoadCreatureCatalog: {}.skills is not an array", creature);
        return false;
    }
    const auto& arr = obj["skills"];
    if (arr.size() > static_cast<size_t>(kMaxSkills)) {
        spdlog::warn("LoadCreatureCatalog: {} grants more than {} skills", creature, kMaxSkills);
        return false;
    }
    CharacterDesc scratch = d;
    scratch.skill_grant_count = 0;
    for (const auto& entry : arr) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
            spdlog::warn("LoadCreatureCatalog: {}.skills entries need a string 'name'", creature);
            return false;
        }
        const std::string name = entry["name"].get<std::string>();
        const SkillId id = SkillIdFromName(name.c_str());
        if (id == SkillId::Count) {
            spdlog::warn("LoadCreatureCatalog: {} grants unknown skill '{}'", creature, name);
            return false;
        }
        int32_t level = 1;
        if (entry.contains("level")) {
            if (!entry["level"].is_number_integer()) {
                spdlog::warn("LoadCreatureCatalog: {}.skills['{}'].level is not an integer",
                             creature, name);
                return false;
            }
            level = entry["level"].get<int32_t>();
            if (level < 1) {
                spdlog::warn("LoadCreatureCatalog: {}.skills['{}'].level {} is below 1",
                             creature, name, level);
                return false;
            }
        }
        scratch.skill_grants[scratch.skill_grant_count++] = {static_cast<int32_t>(id), level};
    }
    d = scratch;
    return true;
}

}  // namespace

bool LoadCreatureCatalog(const std::string& path, CreatureCatalog& out) {
    std::ifstream file(path);
    if (!file.good()) {
        spdlog::warn("LoadCreatureCatalog: cannot open '{}'", path);
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("LoadCreatureCatalog: '{}' is not valid JSON: {}", path, e.what());
        return false;
    }
    if (!j.is_object()) {
        spdlog::warn("LoadCreatureCatalog: '{}' top level is not an object", path);
        return false;
    }

    // Apply onto a copy so a mid-parse error leaves `out` untouched.
    CreatureCatalog next = out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& name = it.key();
        if (!name.empty() && name[0] == '_') {
            continue;  // "_comment"-style keys are documentation, not creatures
        }
        const CreatureId id = CreatureIdFromName(name.c_str());
        if (id == CreatureId::Count) {
            spdlog::warn("LoadCreatureCatalog: '{}' unknown creature '{}'", path, name);
            return false;
        }
        const nlohmann::json& o = it.value();
        if (!o.is_object()) {
            spdlog::warn("LoadCreatureCatalog: '{}' {} must be an object", path, name);
            return false;
        }
        CharacterDesc& d = next.defs[static_cast<int>(id)];
        bool ok = true;
        ok = ok && ReadNum(o, name.c_str(), "hp", d.hp);
        ok = ok && ReadNum(o, name.c_str(), "move_speed", d.move_speed);
        ok = ok && ReadNum(o, name.c_str(), "accuracy", d.accuracy);
        ok = ok && ReadNum(o, name.c_str(), "evasion", d.evasion);
        ok = ok && ReadNum(o, name.c_str(), "defense", d.defense);
        ok = ok && ReadNum(o, name.c_str(), "armour", d.armour);
        ok = ok && ReadNum(o, name.c_str(), "xp_reward", d.xp_reward);
        if (d.attack_count > 0) {
            ok = ok && ReadNum(o, name.c_str(), "attack_damage", d.attacks[0].base_damage);
            ok = ok && ReadNum(o, name.c_str(), "attack_cooldown", d.attacks[0].cooldown);
            ok = ok && ReadNum(o, name.c_str(), "attack_crit", d.attacks[0].crit_chance);
        }
        ok = ok && ReadSkillGrants(o, name.c_str(), d);
        if (!ok) {
            return false;
        }
    }

    out = next;
    return true;
}

}  // namespace badlands
