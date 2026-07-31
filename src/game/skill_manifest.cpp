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

// Read an optional boolean key. Same contract as ReadNum: absent keeps the
// compiled default, present-but-wrong-type fails loudly.
bool ReadBool(const nlohmann::json& obj, const std::string& skill, const char* key,
              bool& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_boolean()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a boolean", skill, key);
        return false;
    }
    dst = obj[key].get<bool>();
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

// Read the optional "constants" object: skill-specific tuning numbers, the
// ONLY thing this manifest is allowed to carry (behaviour stays in code -- see
// SkillSpec's own doc comment). A name absent from the compiled defaults is
// APPENDED rather than rejected: the manifest is the tuning surface, and a
// skill's code decides which names it reads.
bool ReadConstants(const nlohmann::json& obj, const std::string& skill, SkillSpec& s) {
    if (!obj.contains("constants")) {
        return true;
    }
    if (!obj["constants"].is_object()) {
        spdlog::warn("LoadSkillCatalog: {}.constants is not an object", skill);
        return false;
    }
    for (const auto& [key, value] : obj["constants"].items()) {
        if (!value.is_number()) {
            spdlog::warn("LoadSkillCatalog: {}.constants.{} is not a number", skill, key);
            return false;
        }
        const float v = value.get<float>();
        bool replaced = false;
        for (int32_t i = 0; i < s.constant_count && i < kMaxSkillConstants; ++i) {
            if (s.constants[i].name == key) {
                s.constants[i].value = v;
                replaced = true;
                break;
            }
        }
        if (replaced) {
            continue;
        }
        if (s.constant_count >= kMaxSkillConstants) {
            // Failing loudly rather than dropping: a designer who authored a
            // ninth constant is missing one silently otherwise.
            spdlog::warn("LoadSkillCatalog: {} has more than {} constants", skill,
                         kMaxSkillConstants);
            return false;
        }
        s.constants[s.constant_count].name = key;
        s.constants[s.constant_count].value = v;
        ++s.constant_count;
    }
    return true;
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
        int32_t trigger = static_cast<int32_t>(s.trigger);
        int32_t target = static_cast<int32_t>(s.target);
        int32_t attack_test = static_cast<int32_t>(s.attack_test);
        const bool ok =
            ReadChoice(o, name, "trigger",
                       {{"action", 0}, {"passive", 1}, {"intention", 2}}, trigger) &&
            ReadChoice(o, name, "target",
                       {{"none", 0}, {"self", 1}, {"any", 2}, {"multi", 3}, {"point", 4}},
                       target) &&
            ReadChoice(o, name, "attack_test",
                       {{"none", 0}, {"melee", 1}, {"ranged", 2}}, attack_test) &&
            ReadNum(o, name, "target_limit", s.target_limit) &&
            ReadNum(o, name, "cooldown", s.cooldown_seconds) &&
            ReadNum(o, name, "intention_duration", s.intention_duration_seconds) &&
            ReadBool(o, name, "castable_in_melee", s.castable_in_melee) &&
            ReadBool(o, name, "guaranteed_test", s.guaranteed_test) &&
            ReadString(o, name, "effect", s.effect) &&
            ReadConstants(o, name, s);
        if (!ok) {
            return false;
        }
        s.trigger = static_cast<SkillTrigger>(trigger);
        s.target = static_cast<SkillTargetMode>(target);
        s.attack_test = static_cast<SkillAttackTest>(attack_test);
    }

    out = parsed;
    return true;
}

}  // namespace badlands
