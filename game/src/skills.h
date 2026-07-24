// The skill vocabulary tables: what skills exist (SkillCatalog), what
// condition recommends each one (SkillTriggerKind + trigger_param), and who
// learns what at which level (SkillGrantTable). Pure data + pure helpers,
// activity_catalog.cpp's pattern; consumers read the catalog instead of
// hardcoding a switch.

#pragma once

#include "badlands_sim.hpp"
#include "components.h"  // Skills
#include "behaviours/world_view.h"  // PerceivedThreat

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
};

// Dense, indexed by SkillId (static_assert-pinned).
std::span<const SkillDef> SkillDefs();
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

// What the host tells a brain about each learned skill: `ready` = off
// cooldown, `recommended` = the skill's trigger condition currently holds.
// ADVICE, not a command -- slice 2 copies this into the wasm view and the
// brain makes the final call. Pure over its inputs (unit-testable; identical
// live and on replay).
struct SkillContext {
    float health_frac = 1.0f;
    const PerceivedThreat* threats = nullptr;  // nearest-first (WorldView contract)
    int32_t threat_count = 0;
};

struct SkillRecommendation {
    SkillId id;
    bool ready;
    bool recommended;
};

// Fills out[0 .. Skills.count); returns Skills.count.
int32_t evaluate_skill_triggers(const Skills& s, const SkillContext& ctx,
                                SkillRecommendation out[kMaxSkills]);

}  // namespace badlands
