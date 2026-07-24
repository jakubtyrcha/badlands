#include "skills.h"

#include <array>

namespace badlands {

namespace {

constexpr std::array<SkillDef, static_cast<size_t>(kSkillCount)> kSkills{{
    {SkillId::Calcify, "Calcify", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/3.0f, /*cooldown=*/20.0f},
}};

constexpr bool skills_dense() {
    for (size_t i = 0; i < kSkills.size(); ++i) {
        if (static_cast<int32_t>(kSkills[i].id) != static_cast<int32_t>(i)) {
            return false;
        }
    }
    return true;
}
static_assert(skills_dense(), "SkillCatalog must be indexed by SkillId");

constexpr std::array<SkillGrant, 1> kGrants{{
    {HERO_APPRENTICE, 5, SkillId::Calcify},
}};

}  // namespace

std::span<const SkillDef> SkillCatalog() { return kSkills; }

const SkillDef& SkillDefOf(SkillId id) {
    const int32_t i = static_cast<int32_t>(id);
    if (i < 0 || i >= kSkillCount) {
        return kSkills[0];
    }
    return kSkills[static_cast<size_t>(i)];
}

const char* SkillName(int32_t id) {
    if (id < 0 || id >= kSkillCount) {
        return "-";
    }
    return kSkills[static_cast<size_t>(id)].name;
}

std::span<const SkillGrant> SkillGrantTable() { return kGrants; }

bool learn_skill(Skills& s, SkillId id) {
    for (int32_t i = 0; i < s.count; ++i) {
        if (s.ids[i] == id) {
            return false;
        }
    }
    if (s.count >= kMaxSkills) {
        return false;
    }
    s.ids[s.count] = id;
    s.cooldown_remaining[s.count] = 0.0f;
    ++s.count;
    return true;
}

void grant_skills_for_level(Skills& s, int32_t hero_class, int32_t level) {
    for (const SkillGrant& g : SkillGrantTable()) {
        if (g.hero_class == hero_class && g.level == level) {
            learn_skill(s, g.skill);
        }
    }
}

}  // namespace badlands
