#include "game/creature_manifest.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Read an optional numeric key into `dst`. Returns false only on a present-but-
// non-numeric value (a typo worth failing on); a missing key is fine.
bool ReadNum(const nlohmann::json& obj, const char* creature, const char* key, float& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number()) {
        spdlog::warn("LoadCreatureCatalog: {}.{} is not a number", creature, key);
        return false;
    }
    dst = obj[key].get<float>();
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
        if (d.attack_count > 0) {
            ok = ok && ReadNum(o, name.c_str(), "attack_damage", d.attacks[0].base_damage);
            ok = ok && ReadNum(o, name.c_str(), "attack_cooldown", d.attacks[0].cooldown);
            ok = ok && ReadNum(o, name.c_str(), "attack_crit", d.attacks[0].crit_chance);
        }
        if (!ok) {
            return false;
        }
    }

    out = next;
    return true;
}

}  // namespace badlands
