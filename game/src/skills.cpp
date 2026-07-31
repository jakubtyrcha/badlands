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
    {SkillId::Curse, "Curse", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/7.0f},
    {SkillId::DressWounds, "DressWounds", SkillTriggerKind::LowHealth,
     /*trigger_param=*/0.5f},
    {SkillId::Backstab, "Backstab", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/2.0f},
    // Recommended while something is still at a distance -- sneaking is the
    // APPROACH, so the moment it is worth doing is before contact, not in it.
    {SkillId::Sneak, "Sneak", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/20.0f},
    // Recommended at its own long reach: the shot exists to be taken from
    // further away than anything can answer.
    {SkillId::PrecisionShot, "PrecisionShot", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/30.0f},
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
    // The apprentice's ward: it hardens its own hide for a while. ARMOUR only,
    // which is what keeps it a ward rather than a general buff -- a calcified
    // apprentice is still as easy to hit, its blows just land on stone.
    SkillSpec& calcify = specs[static_cast<size_t>(SkillId::Calcify)];
    calcify.trigger = SkillTrigger::Action;
    calcify.target = SkillTargetMode::SelfOnly;
    calcify.attack_test = SkillAttackTest::None;  // a ward, not a blow
    calcify.intention_duration_seconds = 0.0f;    // instant
    calcify.cooldown_seconds = 20.0f;
    calcify.effect = "Hardens the skin to stone; blows land, and glance.";
    set_constant(calcify, "duration_seconds", 30.0f);

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

    // The apprentice's opener. Declares NO attack test -- a curse always
    // lands, because the debuff IS the point and a coin-flip on it would make
    // the class's one level-1 tool unreliable. Declaring no test means the
    // reach comes from the optional "range" constant instead of a weapon
    // (skill_cast.h); 7 keeps it from outreaching the bolt it supports.
    SkillSpec& curse = specs[static_cast<size_t>(SkillId::Curse)];
    curse.trigger = SkillTrigger::Action;
    curse.target = SkillTargetMode::Any;
    curse.target_limit = 1;
    curse.attack_test = SkillAttackTest::None;
    curse.cooldown_seconds = 15.0f;
    curse.intention_duration_seconds = 0.0f;
    curse.effect = "Saps the target's guard: less accurate, and its armour turns brittle.";
    set_constant(curse, "duration_seconds", 8.0f);
    set_constant(curse, "range", 7.0f);

    // The hunter's sustain. SelfOnly, so the engine refuses it at anyone else
    // outright rather than remapping.
    SkillSpec& dress = specs[static_cast<size_t>(SkillId::DressWounds)];
    dress.trigger = SkillTrigger::Action;
    dress.target = SkillTargetMode::SelfOnly;
    dress.attack_test = SkillAttackTest::None;
    dress.cooldown_seconds = 30.0f;
    dress.intention_duration_seconds = 0.0f;
    dress.effect = "Binds a wound in the field; slow to come round again.";
    set_constant(dress, "heal_amount", 8.0f);

    // The grave robber's payoff for not being seen coming. Borrows the melee
    // test (and so the blades' reach), and pays out only against something
    // that is not currently fighting the caster.
    SkillSpec& stab = specs[static_cast<size_t>(SkillId::Backstab)];
    stab.trigger = SkillTrigger::Action;
    stab.target = SkillTargetMode::Any;
    stab.target_limit = 1;
    stab.attack_test = SkillAttackTest::Melee;
    stab.cooldown_seconds = 10.0f;
    stab.intention_duration_seconds = 0.0f;
    stab.effect = "A blade between the ribs -- devastating against someone facing elsewhere.";
    set_constant(stab, "bonus_damage", 6.0f);

    // The grave robber's approach. Long enough to cross an arena, and it ends
    // the moment its owner does anything aggressive rather than only on the
    // timer -- so the duration is a ceiling, not a plan.
    //
    // castable_in_melee = false is the one rule that keeps it an OPENER: you
    // cannot vanish out of a fight somebody is already in with you.
    SkillSpec& sneak = specs[static_cast<size_t>(SkillId::Sneak)];
    sneak.trigger = SkillTrigger::Action;
    sneak.target = SkillTargetMode::SelfOnly;
    sneak.attack_test = SkillAttackTest::None;
    sneak.castable_in_melee = false;
    sneak.cooldown_seconds = 25.0f;
    sneak.intention_duration_seconds = 0.0f;
    sneak.effect = "Slips out of sight; the next blow comes from nowhere.";
    set_constant(sneak, "duration_seconds", 20.0f);

    // The FOCUS, and the only skill that executes SkillTrigger::Intention: two
    // seconds of standing still, then a shot that cannot be blocked or dodged
    // and always crits. Its "range" outreaches every bow in the game, which is
    // why skill_cast_range lets an authored range beat the weapon it borrows
    // the test from.
    //
    // The wind-up IS the cost. Nothing is captured when the focus begins, so a
    // target that dies, walks away, or goes unseen in those two seconds simply
    // gets no shot at all (game/src/skill_focus.h).
    SkillSpec& shot = specs[static_cast<size_t>(SkillId::PrecisionShot)];
    shot.trigger = SkillTrigger::Intention;
    shot.target = SkillTargetMode::Any;
    shot.target_limit = 1;
    shot.attack_test = SkillAttackTest::Ranged;
    shot.guaranteed_test = true;
    shot.cooldown_seconds = 15.0f;
    shot.intention_duration_seconds = 2.0f;
    shot.effect = "Draws a long breath, and does not miss.";
    set_constant(shot, "range", 30.0f);
    set_constant(shot, "crit_multiplier", 3.0f);
}

SkillId SkillIdFromName(const char* name) {
    for (int32_t i = 0; i < kSkillCount; ++i) {
        if (std::strcmp(name, kSkills[static_cast<size_t>(i)].name) == 0) {
            return static_cast<SkillId>(i);
        }
    }
    return SkillId::Count;
}

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

void grant_skills_for_level(Skills& s, const SkillGrants& grants, int32_t level) {
    for (int32_t i = 0; i < grants.count && i < kMaxSkills; ++i) {
        const SkillGrantRow& row = grants.rows[i];
        if (row.level != level) {
            continue;
        }
        if (row.skill < 0 || row.skill >= kSkillCount) {
            continue;  // an empty or out-of-range row (sanitized input, not trusted)
        }
        learn_skill(s, static_cast<SkillId>(row.skill));
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

// Calcify: a plain timed ward on the caster. SelfOnly, so the single target
// the engine resolved IS the caster and this never has to name it. How much
// armour a calcification is worth lives with the STATUS (combat.cpp), not
// here -- the constant this reads is its duration.
void calcify_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float seconds = skill_constant(ctx, "duration_seconds", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        push_effect_op(out, BlSkillEffectOp{BL_FX_APPLY_STATUS, ctx.targets[i].slot,
                                            static_cast<int32_t>(StatusKind::Calcified),
                                            seconds * 1000.0f});
    }
}

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

// Curse: no attack test was declared, so nothing was rolled and there is
// nothing to gate on -- it always lands. The engine still decides WHO may be
// targeted and whether the op is legal; this only says what to ask for.
void curse_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float seconds = skill_constant(ctx, "duration_seconds", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        push_effect_op(out, BlSkillEffectOp{BL_FX_APPLY_STATUS, ctx.targets[i].slot,
                                            static_cast<int32_t>(StatusKind::Cursed),
                                            seconds * 1000.0f});
    }
}

// DressWounds: the first effect to emit BL_FX_HEAL. SelfOnly, so the single
// target the engine resolved IS the caster -- this never has to name it.
void dress_wounds_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float amount = skill_constant(ctx, "heal_amount", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        push_effect_op(out,
                       BlSkillEffectOp{BL_FX_HEAL, ctx.targets[i].slot, 0, amount});
    }
}

// Backstab: the first effect to emit BL_FX_DAMAGE, which has existed and
// applied since the skills slice with nothing shipping that used it.
//
// The engine rolled the melee test and reports what it produced; this adds the
// bonus only against a target NOT currently engaging the caster
// (BlSkillTarget::engaging_caster, which the engine fills -- a guest script
// could never work it out). Against someone already fighting back it is an
// ordinary blow, which is the whole point of the skill.
void backstab_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float bonus = skill_constant(ctx, "bonus_damage", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        const BlSkillTarget& t = ctx.targets[i];
        if (t.attack_test != BL_TEST_HIT) {
            continue;  // blocked or dodged: a knife that missed adds nothing
        }
        const float damage = t.test_damage + (t.engaging_caster != 0 ? 0.0f : bonus);
        push_effect_op(out, BlSkillEffectOp{BL_FX_DAMAGE, t.slot, 0, damage});
    }
}

// Sneak: applies the status and stops. Everything that makes it interesting --
// being unperceivable, the bonus on the blow that ends it, the fact that the
// blow ends it at all -- belongs to the STATUS and is enforced where those
// decisions already happen (combat.cpp, perception.cpp, strike.cpp). This is
// the status subsystem's whole design showing through: an effect starts a
// timer, and the systems that care ask.
void sneak_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float seconds = skill_constant(ctx, "duration_seconds", 0.0f);
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        push_effect_op(out, BlSkillEffectOp{BL_FX_APPLY_STATUS, ctx.targets[i].slot,
                                            static_cast<int32_t>(StatusKind::Sneaking),
                                            seconds * 1000.0f});
    }
}

// PrecisionShot: the engine already rolled a test that could not fail, so the
// whole effect is "deal what that produced". No conditional, no bonus of its
// own -- everything that makes the shot special (it lands, it crits, it
// reaches) is engine-checked DATA on the spec, which is what keeps a
// guaranteed hit from being something an effect can grant itself.
void precision_shot_effect(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    for (int32_t i = 0; i < ctx.target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        const BlSkillTarget& t = ctx.targets[i];
        if (t.attack_test != BL_TEST_HIT || t.test_damage <= 0.0f) {
            continue;  // armour ate it, or the host declined the test entirely
        }
        push_effect_op(out, BlSkillEffectOp{BL_FX_DAMAGE, t.slot, 0, t.test_damage});
    }
}

constexpr std::array<SkillEffectFn, static_cast<size_t>(kSkillCount)> kEffects{{
    calcify_effect,
    shield_bash_effect,
    curse_effect,
    dress_wounds_effect,
    backstab_effect,
    sneak_effect,
    precision_shot_effect,
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
