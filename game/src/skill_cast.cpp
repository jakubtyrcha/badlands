#include "skill_cast.h"

#include "behaviours/rng.h"  // seed_of -- the sim's only randomness
#include "combat.h"      // resolve_attack, effective_combatant, melee_range, ranged_range
#include "components.h"  // Attacks, Skills, Health, Position, Team
#include "game_state.h"  // BadlandsGame, entity_for_slot, slot_for_entity, emit_event
#include "skills.h"      // SkillEffectOf
#include "status.h"      // apply_status

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace badlands {

namespace {

// Index of the longest-reach attack of `cat`, or -1 when the caster has none.
// The bash borrows a WEAPON for its test, so which weapon has to be decided
// somewhere; longest reach matches what the range check already promised.
int longest_attack_index(const Attacks& atk, AttackCategory cat) {
    int best = -1;
    float best_range = -1.0f;
    for (int i = 0; i < atk.count && i < kMaxAttacks; ++i) {
        if (atk.defs[i].category == cat && atk.defs[i].range > best_range) {
            best = i;
            best_range = atk.defs[i].range;
        }
    }
    return best;
}

AttackCategory category_of(SkillAttackTest test) {
    return test == SkillAttackTest::Ranged ? AttackCategory::Ranged : AttackCategory::Melee;
}

int32_t relation_of(const entt::registry& reg, entt::entity caster, entt::entity target) {
    if (caster == target) {
        return BL_REL_SELF;
    }
    const auto* a = reg.try_get<Team>(caster);
    const auto* b = reg.try_get<Team>(target);
    if (a != nullptr && b != nullptr && a->id == b->id) {
        return BL_REL_FRIEND;
    }
    return BL_REL_ENEMY;
}

float health_frac_of(const entt::registry& reg, entt::entity e) {
    const auto* h = reg.try_get<Health>(e);
    if (h == nullptr || h->max_hp <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(h->hp / h->max_hp, 0.0f, 1.0f);
}

// Copies a constant name into the wire's fixed buffer, always NUL-terminated.
void copy_name(char (&dst)[BL_SKILL_NAME_LEN], const std::string& src) {
    const size_t n = std::min(src.size(), static_cast<size_t>(BL_SKILL_NAME_LEN - 1));
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

}  // namespace

float skill_cast_range(const entt::registry& reg, entt::entity caster,
                       const SkillSpec& spec) {
    if (spec.attack_test == SkillAttackTest::None) {
        return spec.constant("range", 0.0f);
    }
    const auto* atk = (caster != entt::null && reg.valid(caster))
                          ? reg.try_get<Attacks>(caster)
                          : nullptr;
    if (atk == nullptr) {
        return 0.0f;
    }
    return spec.attack_test == SkillAttackTest::Ranged ? ranged_range(*atk)
                                                       : melee_range(*atk);
}

bool validate_cast(const BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
                   uint32_t named_target_slot, CastPlan& out) {
    const entt::registry& reg = game.registry;
    const entt::entity caster = entity_for_slot(game, static_cast<int32_t>(caster_slot));
    const auto* skills = (caster != entt::null) ? reg.try_get<Skills>(caster) : nullptr;
    if (skills == nullptr || !reg.all_of<Position>(caster)) {
        spdlog::warn("[skill] slot {}: no live/skilled caster, cast dropped", caster_slot);
        return false;
    }
    if (skill_index < 0 || skill_index >= skills->count || skill_index >= kMaxSkills) {
        spdlog::warn("[skill] slot {}: skill index {} out of range (count {}), cast dropped",
                     caster_slot, skill_index, skills->count);
        return false;
    }
    if (skills->cooldown_remaining[skill_index] > 0.0f) {
        spdlog::warn("[skill] slot {}: skill {} still cooling down ({:.1f}s), cast dropped",
                     caster_slot, SkillName(static_cast<int32_t>(skills->ids[skill_index])),
                     skills->cooldown_remaining[skill_index]);
        return false;
    }

    const SkillId id = skills->ids[skill_index];
    const SkillSpec& spec = game.skills.specs[static_cast<size_t>(id)];
    if (spec.trigger != SkillTrigger::Action) {
        // Declared vocabulary the engine does not execute yet. Refused, never
        // approximated as an action -- a passive that silently fired on demand
        // would be a different skill than the one authored.
        spdlog::warn("[skill] slot {}: {} is not an action skill (trigger {}), cast dropped",
                     caster_slot, SkillName(static_cast<int32_t>(id)),
                     static_cast<int32_t>(spec.trigger));
        return false;
    }

    // Target resolution by mode.
    out = CastPlan{};
    out.caster = caster;
    out.id = id;
    out.spec = &spec;
    switch (spec.target) {
        case SkillTargetMode::None:
            break;  // affects nobody; an effect may still act on the caster's own state
        case SkillTargetMode::SelfOnly: {
            if (named_target_slot != UINT32_MAX && named_target_slot != caster_slot) {
                spdlog::warn("[skill] slot {}: {} targets self only; slot {} refused",
                             caster_slot, SkillName(static_cast<int32_t>(id)),
                             named_target_slot);
                return false;
            }
            out.targets[out.target_count++] = caster_slot;
            break;
        }
        case SkillTargetMode::Any: {
            const entt::entity target =
                entity_for_slot(game, static_cast<int32_t>(named_target_slot));
            if (target == entt::null || !reg.all_of<Position>(target)) {
                spdlog::warn("[skill] slot {}: {} names no live target, cast dropped",
                             caster_slot, SkillName(static_cast<int32_t>(id)));
                return false;
            }
            out.targets[out.target_count++] = named_target_slot;
            break;
        }
        case SkillTargetMode::Multi:
        case SkillTargetMode::Point:
            spdlog::warn("[skill] slot {}: {} uses targeting mode {}, which this build does "
                         "not resolve yet; cast dropped",
                         caster_slot, SkillName(static_cast<int32_t>(id)),
                         static_cast<int32_t>(spec.target));
            return false;
        default:
            // Unreachable through the manifest (ReadChoice only accepts named
            // values) and through sanitize_skill_catalog, but Sim::SetSkillCatalog
            // is public API: refuse rather than fall through having resolved
            // nobody, which would otherwise report success and stamp a cooldown.
            spdlog::warn("[skill] slot {}: {} has an unknown targeting mode {}, cast dropped",
                         caster_slot, SkillName(static_cast<int32_t>(id)),
                         static_cast<int32_t>(spec.target));
            return false;
    }

    // Reach, against every resolved target (the caster itself is trivially in
    // range, so a SelfOnly cast never fails this). A non-positive reach means
    // UNBOUNDED, not "zero reach": a skill that declares no attack test and
    // authors no "range" constant is deliberately unranged (skill_cast.h), and
    // treating 0 as a distance would refuse every target except the caster.
    const float reach = skill_cast_range(reg, caster, spec);
    const bool ranged_check = reach > 0.0f;
    const glm::vec2 from = reg.get<Position>(caster).pos;
    for (int32_t i = 0; i < out.target_count; ++i) {
        const entt::entity t = entity_for_slot(game, static_cast<int32_t>(out.targets[i]));
        if (t == entt::null) {
            return false;
        }
        if (t == caster) {
            continue;
        }
        if (ranged_check && glm::distance(from, reg.get<Position>(t).pos) > reach) {
            spdlog::warn("[skill] slot {}: {} is out of reach ({:.1f}), cast dropped",
                         caster_slot, SkillName(static_cast<int32_t>(id)), reach);
            return false;
        }
    }
    return true;
}

BlSkillCastContext build_cast_context(const BadlandsGame& game, entt::entity caster,
                                      SkillId id, const SkillSpec& spec,
                                      const uint32_t target_slots[], int32_t target_count) {
    const entt::registry& reg = game.registry;
    BlSkillCastContext ctx{};
    ctx.version = BL_SKILL_ABI_VERSION;
    ctx.skill_id = static_cast<int32_t>(id);
    ctx.world_millis = game.world_millis;

    const uint32_t caster_slot = slot_for_entity(game, caster);
    const Combatant caster_stats =
        reg.all_of<Combatant>(caster) ? reg.get<Combatant>(caster) : Combatant{};
    const Attacks* atk = reg.try_get<Attacks>(caster);
    const glm::vec2 from = reg.get<Position>(caster).pos;

    ctx.caster.slot = caster_slot;
    ctx.caster.pos_x = from.x;
    ctx.caster.pos_z = from.y;
    ctx.caster.accuracy = caster_stats.accuracy;
    ctx.caster.health_frac = health_frac_of(reg, caster);
    ctx.caster.melee_range = atk != nullptr ? melee_range(*atk) : 0.0f;
    ctx.caster.ranged_range = atk != nullptr ? ranged_range(*atk) : 0.0f;

    // The seed an effect may draw from: the same identity axes the combat
    // rolls use (who, when, which skill), so it is replay-reproducible.
    ctx.seed = seed_of(caster_slot, game.world_millis) ^
               seed_of(static_cast<uint32_t>(kSkillSeedBase + ctx.skill_id), game.world_millis);

    // The declared attack test, rolled ONCE per target, here -- an effect
    // never rolls (skill_abi.h). The seed's attack_index axis is offset by
    // kSkillSeedBase so this roll is independent of any weapon swing landing
    // in the same tick against the same target.
    const int test_attack =
        (spec.attack_test != SkillAttackTest::None && atk != nullptr)
            ? longest_attack_index(*atk, category_of(spec.attack_test))
            : -1;

    for (int32_t i = 0; i < target_count && i < BL_SKILL_MAX_TARGETS; ++i) {
        const entt::entity t = entity_for_slot(game, static_cast<int32_t>(target_slots[i]));
        if (t == entt::null) {
            continue;
        }
        const Combatant def = effective_combatant(reg, t);
        BlSkillTarget& row = ctx.targets[ctx.target_count];
        row.slot = target_slots[i];
        const glm::vec2 tp = reg.get<Position>(t).pos;
        row.pos_x = tp.x;
        row.pos_z = tp.y;
        row.dist = glm::distance(from, tp);
        row.health_frac = health_frac_of(reg, t);
        row.defense = def.defense;
        row.evasion = def.evasion;
        row.armour = def.armour;
        row.relation = relation_of(reg, caster, t);
        row.attack_test = BL_TEST_NOT_RUN;
        row.test_damage = 0.0f;

        if (test_attack >= 0) {
            CombatRequest req;
            req.attacker = caster_stats;
            req.attack = atk->defs[test_attack];
            req.defender = def;
            req.attacker_slot = caster_slot;
            req.target_slot = target_slots[i];
            req.world_millis = game.world_millis;
            req.attack_index = kSkillSeedBase + ctx.skill_id;
            const CombatResult res = resolve_attack(req);
            row.attack_test = res.blocked  ? BL_TEST_BLOCKED
                              : res.dodged ? BL_TEST_DODGED
                                           : BL_TEST_HIT;
            row.test_damage = res.damage;
        }
        ++ctx.target_count;
    }

    for (int32_t i = 0; i < spec.constant_count && i < BL_SKILL_MAX_CONSTANTS; ++i) {
        copy_name(ctx.constants[ctx.constant_count].name, spec.constants[i].name);
        ctx.constants[ctx.constant_count].value = spec.constants[i].value;
        ++ctx.constant_count;
    }
    return ctx;
}

void apply_effect_batch(BadlandsGame& game, uint32_t caster_slot,
                        const BlSkillCastContext& ctx, const BlSkillEffectBatch& batch) {
    for (int32_t i = 0; i < batch.count && i < BL_SKILL_MAX_OPS; ++i) {
        const BlSkillEffectOp& op = batch.ops[i];

        // An op may only reach an entity the engine itself put in the context.
        // This is the bound on a scripted effect: it cannot name a slot it was
        // never shown, however it came by the number.
        bool in_context = false;
        for (int32_t k = 0; k < ctx.target_count && k < BL_SKILL_MAX_TARGETS; ++k) {
            if (ctx.targets[k].slot == op.target_slot) {
                in_context = true;
                break;
            }
        }
        if (!in_context) {
            spdlog::warn("[skill] effect op names slot {}, which is not one of its targets; "
                         "dropped", op.target_slot);
            continue;
        }
        const entt::entity target = entity_for_slot(game, static_cast<int32_t>(op.target_slot));
        if (target == entt::null) {
            continue;  // died between the effect running and the batch applying
        }

        switch (op.kind) {
            case BL_FX_APPLY_STATUS: {
                if (op.param_i <= static_cast<int32_t>(StatusKind::None) ||
                    op.param_i >= kStatusKindCount) {
                    spdlog::warn("[skill] effect asked for unknown status {}, dropped",
                                 op.param_i);
                    break;
                }
                // Non-finite/negative durations are simply not applied
                // (apply_status treats <= 0 as a no-op), so a missing constant
                // costs the cast rather than freezing the target forever.
                const int64_t millis =
                    std::isfinite(op.param_f) ? static_cast<int64_t>(op.param_f) : 0;
                apply_status(game, target, static_cast<StatusKind>(op.param_i), millis,
                             caster_slot);
                break;
            }
            case BL_FX_DAMAGE: {
                auto* h = game.registry.try_get<Health>(target);
                if (h == nullptr || !std::isfinite(op.param_f) || op.param_f <= 0.0f) {
                    break;
                }
                h->hp -= op.param_f;
                emit_char_hit(game, caster_slot, op.target_slot, op.param_f, h->hp,
                              game.registry.get<Position>(target).pos);
                break;
            }
            default:
                spdlog::warn("[skill] unrecognized effect op kind {}, skipped", op.kind);
                break;
        }
    }
}

void run_cast(BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
              const CastPlan& plan) {
    const SkillSpec& spec = *plan.spec;
    const BlSkillCastContext ctx = build_cast_context(game, plan.caster, plan.id, spec,
                                                      plan.targets, plan.target_count);
    BlSkillEffectBatch batch{};
    SkillEffectOf(plan.id)(ctx, batch);

    // The cooldown is stamped whether or not the effect asked for anything: a
    // bash that was blocked still happened. Only the SKILL's own timer moves --
    // the weapon whose test it borrowed stays ready.
    game.registry.get<Skills>(plan.caster).cooldown_remaining[skill_index] =
        spec.cooldown_seconds;

    const glm::vec2 at = game.registry.get<Position>(plan.caster).pos;
    emit_event(game, GameEvent{.kind = GameEventKind::SkillUsed,
                               .actor_id = caster_slot,
                               .target_id = plan.target_count > 0 ? plan.targets[0]
                                                                  : caster_slot,
                               .target_kind = kEventTargetCharacter,
                               .amount = static_cast<float>(plan.id),
                               .x = at.x,
                               .z = at.y,
                               .at_millis = game.world_millis});

    apply_effect_batch(game, caster_slot, ctx, batch);
}

}  // namespace badlands
