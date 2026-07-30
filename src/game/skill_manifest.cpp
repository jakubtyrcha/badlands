#include "game/skill_manifest.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Read an optional numeric key into `dst` (creature_manifest's contract).
template <typename T>
bool ReadNum(const nlohmann::json& obj, const std::string& skill, const char* key, T& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a number", skill, key);
        return false;
    }
    dst = obj[key].get<T>();
    return true;
}

bool ReadString(const nlohmann::json& obj, const std::string& skill, const char* key,
                std::string& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a string", skill, key);
        return false;
    }
    dst = obj[key].get<std::string>();
    return true;
}

// Read an optional named-choice key: the value must be one of `choices`'
// names; its paired int lands in `dst`. Unknown names fail loudly (silently
// keeping a default would be indistinguishable from a typo doing nothing).
bool ReadChoice(const nlohmann::json& obj, const std::string& skill, const char* key,
                std::initializer_list<std::pair<const char*, int32_t>> choices,
                int32_t& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a string", skill, key);
        return false;
    }
    const std::string v = obj[key].get<std::string>();
    for (const auto& [name, value] : choices) {
        if (v == name) {
            dst = value;
            return true;
        }
    }
    spdlog::warn("LoadSkillCatalog: {}.{} has unknown value '{}'", skill, key, v);
    return false;
}

}  // namespace

bool LoadSkillCatalog(const std::string& path, SkillCatalog& out) {
    std::ifstream file(path);
    if (!file.good()) {
        spdlog::warn("LoadSkillCatalog: cannot open '{}'", path);
        return false;
    }
    nlohmann::json manifest;
    try {
        file >> manifest;
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("LoadSkillCatalog: '{}' is not valid JSON: {}", path, e.what());
        return false;
    }
    if (!manifest.is_object()) {
        spdlog::warn("LoadSkillCatalog: '{}' top level is not an object", path);
        return false;
    }

    // Parse into a scratch copy so a failure half-way leaves `out` untouched.
    SkillCatalog parsed = out;
    for (const auto& [name, o] : manifest.items()) {
        if (!name.empty() && name[0] == '_') {
            continue;  // "_comment"-style keys are documentation, not skills
        }
        const SkillId id = SkillIdFromName(name.c_str());
        if (id == SkillId::Count) {
            spdlog::warn("LoadSkillCatalog: '{}' -> unknown skill '{}'", path, name);
            return false;
        }
        if (!o.is_object()) {
            spdlog::warn("LoadSkillCatalog: '{}' -> {} is not an object", path, name);
            return false;
        }
        SkillSpec& s = parsed.specs[static_cast<size_t>(id)];
        int32_t activation = static_cast<int32_t>(s.activation);
        int32_t targeting = static_cast<int32_t>(s.targeting);
        const bool ok =
            ReadChoice(o, name, "activation", {{"active", 0}, {"passive", 1}}, activation) &&
            ReadChoice(o, name, "targeting", {{"direct", 0}, {"aoe", 1}}, targeting) &&
            ReadNum(o, name, "duration", s.duration_seconds) &&
            ReadNum(o, name, "cooldown", s.cooldown_seconds) &&
            ReadString(o, name, "effect", s.effect);
        if (!ok) {
            return false;
        }
        s.activation = static_cast<SkillActivation>(activation);
        s.targeting = static_cast<SkillTargeting>(targeting);
    }

    out = parsed;
    return true;
}

}  // namespace badlands
