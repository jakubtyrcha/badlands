// The skill vocabulary tables: what skills exist (SkillCatalog), what
// condition recommends each one (SkillTriggerKind + trigger_param), and who
// learns what at which level (SkillGrantTable). Pure data + pure helpers,
// activity_catalog.cpp's pattern; consumers read the catalog instead of
// hardcoding a switch.

#pragma once

#include "badlands_sim.hpp"
#include "components.h"  // Skills

#include <span>

namespace badlands {

// The data-driven trigger vocabulary. Grows as skills need new conditions.
enum class SkillTriggerKind : int32_t {
    MeleeThreatClose = 0,  // a threat within trigger_param world units
    LowHealth,             // health fraction at or below trigger_param
};

struct SkillDef {
    SkillId id;
    const char* name;
    SkillTriggerKind trigger;
    float trigger_param;
    float cooldown;  // seconds between uses (ticked in slice 2)
};

// Dense, indexed by SkillId (static_assert-pinned).
std::span<const SkillDef> SkillCatalog();
// Out-of-range ids resolve to the Calcify row (id 0), mirroring ActivityInfoOf.
const SkillDef& SkillDefOf(SkillId id);

// One row of "class X learns skill Y at level L". Append-only.
struct SkillGrant {
    int32_t hero_class;  // HeroClassId
    int32_t level;
    SkillId skill;
};
std::span<const SkillGrant> SkillGrantTable();

// Dupe-proof append; false when already known or the component is full.
bool learn_skill(Skills& s, SkillId id);
// Applies every grant row matching (hero_class, level) exactly.
void grant_skills_for_level(Skills& s, int32_t hero_class, int32_t level);

}  // namespace badlands
