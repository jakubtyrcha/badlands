#include "skills.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace badlands {

namespace {

constexpr std::array<SkillDef, static_cast<size_t>(kSkillCount)> kSkills{{
    {SkillId::Calcify, "Calcify", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/3.0f},
    {SkillId::ShieldBash, "ShieldBash", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/2.0f},
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

namespace {

// Appends one authored constant; the catalog author is trusted (these are
// compile-time literals), so an overflow is a programming error, not input.
void set_constant(SkillSpec& s, const char* name, float value) {
    if (s.constant_count >= kMaxSkillConstants) {
        return;
    }
    s.constants[s.constant_count].name = name;
    s.constants[s.constant_count].value = value;
    ++s.constant_count;
}

}  // namespace

float SkillSpec::constant(const char* name, float fallback) const {
    if (name == nullptr) {
        return fallback;
    }
    for (int32_t i = 0; i < constant_count && i < kMaxSkillConstants; ++i) {
        if (constants[i].name == name) {
            return constants[i].value;
        }
    }
    return fallback;
}

SkillCatalog::SkillCatalog() {
    SkillSpec& calcify = specs[static_cast<size_t>(SkillId::Calcify)];
    calcify.trigger = SkillTrigger::Action;
    calcify.target = SkillTargetMode::SelfOnly;
    calcify.attack_test = SkillAttackTest::None;  // a ward, not a blow
    calcify.intention_duration_seconds = 0.0f;    // instant; the shield persists until consumed
    calcify.cooldown_seconds = 20.0f;
    calcify.effect = "Absorbs the next physical strike, then shatters.";

    // The mercenary's control tool: a shield slam that deals nothing and stuns
    // what it lands on. Range comes from attack_test = Melee (the caster's own
    // melee reach), so a mercenary's bash reaches exactly as far as its sword.
    SkillSpec& bash = specs[static_cast<size_t>(SkillId::ShieldBash)];
    bash.trigger = SkillTrigger::Action;
    bash.target = SkillTargetMode::Any;
    bash.target_limit = 1;
    bash.attack_test = SkillAttackTest::Melee;
    bash.cooldown_seconds = 12.0f;
    bash.intention_duration_seconds = 0.0f;
    bash.effect = "Slams the target with a shield; a landed blow leaves it stunned.";
    set_constant(bash, "stun_seconds", 3.0f);
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

// --- effects -----------------------------------------------------------------

float skill_constant(const BlSkillCastContext& ctx, const char* name, float fallback) {
    if (name == nullptr) {
        return fallback;
    }
    for (int32_t i = 0; i < ctx.constant_count && i < BL_SKILL_MAX_CONSTANTS; ++i) {
        if (std::strncmp(ctx.constants[i].name, name, BL_SKILL_NAME_LEN) == 0) {
            return ctx.constants[i].value;
        }
    }
    return fallback;
}

void push_effect_op(BlSkillEffectBatch& out, const BlSkillEffectOp& op) {
    if (out.count >= BL_SKILL_MAX_OPS) {
        spdlog::warn("[skill] effect asked for more than {} ops; extra dropped",
                     BL_SKILL_MAX_OPS);
        return;
    }
    out.ops[out.count++] = op;
}

namespace {

// Calcify: absorbing the next strike needs a status that does not exist yet
// (an absorb charge, not a timer), so this ward has no effect to run. It still
// exists, is still granted, and still displays -- the mechanic is a later
// slice. A no-op rather than a missing table entry, so SkillEffectOf is
// total.
void calcify_effect(const BlSkillCastContext&, BlSkillEffectBatch&) {}

// ShieldBash: PURE CONTROL. The engine has already rolled the melee test this
// skill declared (SkillAttackTest::Melee) against every target; a landed one
// is stunned for the authored number of seconds, and the damage that roll
// produced is deliberately thrown away -- the shield knocks the wind out of
// something, it does not cut it.
void shield_bash_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float seconds = skill_constant(ctx, "stun_seconds", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        if (ctx.targets[i].attack_test != BL_TEST_HIT) {
            continue;  // blocked, dodged, or never rolled -- nothing to stun
        }
        push_effect_op(out, BlSkillEffectOp{BL_FX_APPLY_STATUS, ctx.targets[i].slot,
                                            static_cast<int32_t>(StatusKind::Stunned),
                                            seconds * 1000.0f});
    }
}

constexpr std::array<SkillEffectFn, static_cast<size_t>(kSkillCount)> kEffects{{
    calcify_effect,
    shield_bash_effect,
}};

}  // namespace

SkillEffectFn SkillEffectOf(SkillId id) {
    const int32_t i = static_cast<int32_t>(id);
    if (i < 0 || i >= kSkillCount) {
        return kEffects[0];
    }
    return kEffects[static_cast<size_t>(i)];
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
