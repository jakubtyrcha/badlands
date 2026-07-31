// The skill vocabulary tables: what skills exist (SkillCatalog), what
// condition recommends each one (SkillTriggerKind + trigger_param), and who
// learns what at which level (SkillGrantTable). Pure data + pure helpers,
// activity_catalog.cpp's pattern; consumers read the catalog instead of
// hardcoding a switch.

#pragma once

#include "badlands_sim.hpp"
#include "components.h"  // Skills
#include "skill_abi.h"   // BlSkillCastContext / BlSkillEffectBatch -- the effect contract
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

// --- effects: the pure half of a cast ---------------------------------------
// One function per skill, and the ONLY thing a skill's own author writes. It
// reads a flat context and appends ops (game/src/skill_abi.h) -- it cannot
// touch the registry, cannot roll, and cannot apply anything itself. That is
// what lets the same function be C++ today and a wasm export later: the
// signature already is the wire.

using SkillEffectFn = void (*)(const BlSkillCastContext&, BlSkillEffectBatch&);

// Dense, indexed by SkillId (static_assert-pinned like SkillDefs). Never null:
// a skill whose mechanic is not written yet gets a documented no-op.
SkillEffectFn SkillEffectOf(SkillId id);

// Named lookup over a context's constants, `fallback` when absent -- the
// guest-side twin of SkillSpec::constant (the engine copies the spec's
// constants into every context, so both read the same numbers by the same
// names).
float skill_constant(const BlSkillCastContext& ctx, const char* name, float fallback = 0.0f);

// Appends one op; warns and drops at BL_SKILL_MAX_OPS rather than overrunning
// (an effect asking for more than the batch holds is a bug in the effect, and
// silently truncating it would read as "the engine ignored me").
void push_effect_op(BlSkillEffectBatch& out, const BlSkillEffectOp& op);

}  // namespace badlands
