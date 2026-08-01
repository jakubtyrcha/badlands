#include "skill_cast.h"

#include "behaviours/rng.h"  // seed_of -- the sim's only randomness
#include "combat.h"      // resolve_attack, effective_combatant, melee_range, ranged_range
#include "components.h"  // Attacks, Skills, Health, Position, Team
#include "game_state.h"  // BadlandsGame, entity_for_slot, slot_for_entity, emit_event
#include "nav_world.h"   // nav_point_free -- a point cast must land somewhere stand-on-able
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

// Is `target` currently fighting `caster`? Locked with it in melee, or running
// an Attack intention aimed at it. Engine-side because a guest script cannot
// see either -- and because "did I catch this one unaware" is exactly the
// question an effect like Backstab needs answered but must not answer itself.
//
// Conservative on purpose: anything ambiguous reads as ENGAGED, so a bonus for
// catching someone off guard is never paid out on a doubt.
bool engaging_of(const entt::registry& reg, entt::entity caster, entt::entity target) {
    if (caster == target) {
        return true;
    }
    // "Engaging THE CASTER", not "engaged with anybody". The distinction is the
    // whole skill: a grave robber flanking a goblin that is locked with a
    // friendly mercenary is the textbook backstab, and treating any melee lock
    // as engagement would suppress the bonus in exactly the group fight it
    // exists for.
    //
    // Two ways to be fighting this caster specifically:
    //   * in melee contact WITH IT -- locked, and within its own reach of the
    //     caster (MeleeLock records no partner, so the reach test is what names
    //     one);
    //   * chasing it -- an Attack intention whose engagement MoveTarget holds
    //     the caster's entity (enqueue_engage, command.h).
    if (reg.all_of<MeleeLock>(target) && reg.all_of<Position>(target) &&
        reg.all_of<Position>(caster)) {
        const auto* atk = reg.try_get<Attacks>(target);
        const float reach = atk != nullptr ? melee_range(*atk) : 0.0f;
        if (reach > 0.0f && glm::distance(reg.get<Position>(target).pos,
                                          reg.get<Position>(caster).pos) <= reach) {
            return true;
        }
    }
    if (const auto* ci = reg.try_get<CurrentIntention>(target);
        ci != nullptr && ci->kind == IntentionKind::Attack) {
        const auto* mt = reg.try_get<MoveTarget>(target);
        if (mt != nullptr && mt->kind == MoveTarget::Kind::Entity && mt->entity == caster) {
            return true;
        }
    }
    return false;
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
    // An authored "range" wins outright, test or no test. A skill that borrows
    // a weapon's test does not necessarily borrow its reach: a precision shot
    // is thrown with the same bow and lands a great deal further away, and
    // there is no way to say so if the weapon always decides. The weapon
    // remains the DEFAULT -- a bash that authors nothing still cannot outreach
    // the sword it borrows -- which was the property this rule existed for.
    if (const float authored = spec.constant("range", 0.0f); authored > 0.0f) {
        return authored;
    }
    if (spec.attack_test == SkillAttackTest::None) {
        return 0.0f;  // unbounded; see the header
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

bool validate_cast(BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
                   uint32_t named_target_slot, CastPlan& out, SkillTrigger channel,
                   glm::vec2 point) {
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
    if (has_status(reg, caster, StatusKind::Disengaged)) {
        spdlog::warn("[skill] slot {}: disengaged, cast dropped", caster_slot);
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
    if (spec.trigger == SkillTrigger::Passive || spec.trigger != channel) {
        // The channel split, both ways. An Intention skill fired through the
        // action gateway would cost nothing to cast; an Action skill adopted as
        // a focus would idle for a duration it never declared. Passive is still
        // declared vocabulary with no execution at all, so it is refused on
        // every channel -- never approximated as its nearest neighbour.
        spdlog::warn("[skill] slot {}: {} has trigger {} but arrived on channel {}, cast dropped",
                     caster_slot, SkillName(static_cast<int32_t>(id)),
                     static_cast<int32_t>(spec.trigger), static_cast<int32_t>(channel));
        return false;
    }
    if (channel == SkillTrigger::Intention && spec.intention_duration_seconds <= 0.0f) {
        // A focus with no duration is a contradiction, and begin_focus already
        // refuses one -- but refusing it HERE is what stops the loop: without
        // this the cast validates, gets adopted, queues a logged FocusSkill that
        // begin_focus then declines, advance_intentions ends the intention
        // because nothing is running, and the brain re-suggests it on the very
        // next wake. Forever, one command in the replay log per wake.
        spdlog::warn("[skill] slot {}: {} is an intention skill with no duration, cast dropped",
                     caster_slot, SkillName(static_cast<int32_t>(id)));
        return false;
    }
    if (!spec.castable_in_melee && reg.all_of<MeleeLock>(caster)) {
        // Authored data, checked here so BOTH gates enforce it -- you cannot
        // vanish out of a fight somebody is already in with you.
        spdlog::warn("[skill] slot {}: {} cannot be cast in melee contact, cast dropped",
                     caster_slot, SkillName(static_cast<int32_t>(id)));
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
            // ...and it has to be somewhere you can SEE. Target selection and
            // threat perception both skip a sneaking entity already; without
            // this a caster holding a slot from before it vanished could still
            // cast at it, which would be the one way through the imperceptible
            // rule. Never applies to the caster itself -- a sneaking hero may
            // of course still act on itself.
            if (target != caster && has_status(reg, target, StatusKind::Sneaking)) {
                spdlog::warn("[skill] slot {}: {} names a target it cannot perceive, cast dropped",
                             caster_slot, SkillName(static_cast<int32_t>(id)));
                return false;
            }
            out.targets[out.target_count++] = named_target_slot;
            break;
        }
        case SkillTargetMode::Point: {
            // Targets NOBODY -- a point cast affects a place, and the effect
            // gets that place through the context rather than a target list.
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                spdlog::warn("[skill] slot {}: {} names a non-finite point, cast dropped",
                             caster_slot, SkillName(static_cast<int32_t>(id)));
                return false;
            }
            const float reach = skill_cast_range(reg, caster, spec);
            const glm::vec2 from = reg.get<Position>(caster).pos;
            if (reach > 0.0f && glm::distance(from, point) > reach) {
                spdlog::warn("[skill] slot {}: {} point is out of reach ({:.1f}), cast dropped",
                             caster_slot, SkillName(static_cast<int32_t>(id)), reach);
                return false;
            }
            // ...and it has to be somewhere you could stand. This is the whole
            // reason validate_cast is non-const: answering needs a current
            // navmesh (nav_world.h).
            if (!nav_point_free(game, point)) {
                spdlog::warn("[skill] slot {}: {} point ({:.1f}, {:.1f}) is not passable, "
                             "cast dropped",
                             caster_slot, SkillName(static_cast<int32_t>(id)), point.x, point.y);
                return false;
            }
            out.point = point;
            break;
        }
        case SkillTargetMode::Multi:
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
                                      const uint32_t target_slots[], int32_t target_count,
                                      glm::vec2 point) {
    const entt::registry& reg = game.registry;
    BlSkillCastContext ctx{};
    ctx.version = BL_SKILL_ABI_VERSION;
    ctx.skill_id = static_cast<int32_t>(id);
    ctx.point_x = point.x;
    ctx.point_z = point.y;
    ctx.world_ticks = game.world_ticks;

    const uint32_t caster_slot = slot_for_entity(game, caster);
    // effective_combatant, not the raw component -- the same rule declare_strike
    // follows: a cursed caster's skill test rolls at its cursed accuracy.
    const Combatant caster_stats = effective_combatant(reg, caster);
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
    ctx.seed = seed_of(caster_slot, game.world_ticks) ^
               seed_of(static_cast<uint32_t>(kSkillSeedBase + ctx.skill_id), game.world_ticks);

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
        row.engaging_caster = engaging_of(reg, caster, t) ? 1 : 0;
        row.attack_test = BL_TEST_NOT_RUN;
        row.test_damage = 0.0f;

        if (test_attack >= 0) {
            CombatRequest req;
            req.attacker = caster_stats;
            req.attack = atk->defs[test_attack];
            req.defender = def;
            req.attacker_slot = caster_slot;
            req.target_slot = target_slots[i];
            req.world_ticks = game.world_ticks;
            req.attack_index = kSkillSeedBase + ctx.skill_id;
            if (spec.guaranteed_test) {
                // A guaranteed shot is expressed by DIALLING THE GATES, not by
                // bypassing resolve_attack: armour still applies, the damage
                // type still matters, and the whole thing stays one seeded,
                // replayable pipeline. Accuracy 1 against defense 0 cannot
                // miss, evasion 0 cannot dodge, crit_chance 1 always crits.
                req.attacker.accuracy = 1.0f;
                req.attacker.crit_multiplier =
                    std::max(req.attacker.crit_multiplier,
                             spec.constant("crit_multiplier", kBaseCritMultiplier));
                req.defender.defense = 0.0f;
                req.defender.evasion = 0.0f;
                req.attack.crit_chance = 1.0f;
            }
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
        // The CASTER is always in context -- it is right there in ctx.caster,
        // so an op naming it reaches nothing the effect was not shown. This is
        // what lets a targetless cast (None, or a Point cast, which resolves
        // no entities at all) still act on the one who cast it.
        bool in_context = op.target_slot == ctx.caster.slot;
        for (int32_t k = 0; !in_context && k < ctx.target_count && k < BL_SKILL_MAX_TARGETS;
             ++k) {
            if (ctx.targets[k].slot == op.target_slot) {
                in_context = true;
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
                // Hurting somebody else is the other aggressive act (combat.h),
                // so a damaging cast breaks stealth exactly as a swing does --
                // and a self-buff or a heal does not. Checked per op rather
                // than per cast: what makes a skill aggressive is what it did,
                // not what it is called.
                if (op.target_slot != caster_slot) {
                    end_sneak_on_aggression(
                        game, entity_for_slot(game, static_cast<int32_t>(caster_slot)));
                }
                break;
            }
            case BL_FX_HEAL: {
                auto* h = game.registry.try_get<Health>(target);
                if (h == nullptr || !std::isfinite(op.param_f) || op.param_f <= 0.0f) {
                    break;
                }
                // Clamped to the target's own maximum: an effect asking for
                // more than a full heal gets a full heal, never an overheal
                // the rest of the sim has no concept of.
                h->hp = std::min(h->max_hp, h->hp + op.param_f);
                break;
            }
            case BL_FX_TELEPORT: {
                auto* pos = game.registry.try_get<Position>(target);
                if (pos == nullptr) {
                    break;
                }
                // The op named no destination and could not have: it is moved to
                // the CAST'S point, which the engine already checked is in range
                // and stand-on-able. Whatever the effect wrote into param_f/
                // param_i is ignored here, which is the point.
                const glm::vec2 to{ctx.point_x, ctx.point_z};
                if (!std::isfinite(to.x) || !std::isfinite(to.y)) {
                    break;
                }
                pos->pos = to;
                // The route it was walking is abandoned rather than re-planned:
                // its waypoints lead from somewhere this entity no longer is,
                // and plan_paths re-plans from the MoveTarget next tick anyway.
                if (auto* np = game.registry.try_get<NavPath>(target); np != nullptr) {
                    np->waypoints.clear();
                    np->cursor = 0;
                }
                // Contact is broken by arriving somewhere else -- but WITHOUT the
                // disengage penalty, which is charged for walking out under your
                // own steam (movement.h). Blinking is the alternative to that
                // walk, not an instance of it.
                game.registry.remove<MeleeLock>(target);
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
                                                      plan.targets, plan.target_count,
                                                      plan.point);
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
                               .at_ticks = game.world_ticks});

    apply_effect_batch(game, caster_slot, ctx, batch);
}

}  // namespace badlands
