#include "skills.h"

#include <array>
#include <cstring>

namespace badlands {

namespace {

constexpr std::array<SkillDef, static_cast<size_t>(kSkillCount)> kSkills{{
    {SkillId::Calcify, "Calcify", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/3.0f},
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

std::span<const SkillDef> SkillDefs() { return kSkills; }

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

SkillCatalog::SkillCatalog() {
    SkillSpec& calcify = specs[static_cast<size_t>(SkillId::Calcify)];
    calcify.activation = SkillActivation::Active;
    calcify.targeting = SkillTargeting::Direct;
    calcify.duration_seconds = 0.0f;  // instant to apply; the shield persists until consumed
    calcify.cooldown_seconds = 20.0f;
    calcify.effect = "Absorbs the next physical strike, then shatters.";
}

SkillId SkillIdFromName(const char* name) {
    for (int32_t i = 0; i < kSkillCount; ++i) {
        if (std::strcmp(name, kSkills[static_cast<size_t>(i)].name) == 0) {
            return static_cast<SkillId>(i);
        }
    }
    return SkillId::Count;
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

int32_t evaluate_skill_triggers(const Skills& s, const SkillContext& ctx,
                                SkillRecommendation out[kMaxSkills]) {
    for (int32_t i = 0; i < s.count; ++i) {
        const SkillDef& def = SkillDefOf(s.ids[i]);
        bool recommended = false;
        switch (def.trigger) {
            case SkillTriggerKind::MeleeThreatClose:
                // threats are nearest-first, so [0] decides "anything close?".
                recommended = ctx.threat_count > 0 && ctx.threats != nullptr &&
                              ctx.threats[0].dist <= def.trigger_param;
                break;
            case SkillTriggerKind::LowHealth:
                recommended = ctx.health_frac <= def.trigger_param;
                break;
        }
        out[i] = {s.ids[i], s.cooldown_remaining[i] <= 0.0f, recommended};
    }
    return s.count;
}

}  // namespace badlands
