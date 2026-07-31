// CASTING: everything between "somebody wants to use skill N" and "the effect
// has been applied". The engine half of the skill contract -- validation,
// target resolution, the pre-rolled attack test, and applying the ops an
// effect asked for.
//
// Deliberately NOT in skills.h: that file is the pure vocabulary (catalog,
// effects, advice) and links standalone into the manifest test target. This
// file is the part that needs a world.
//
// The division of labour, which is the whole point of the contract:
//   ENGINE (here)  -- who may be targeted, whether the caster may cast at all,
//                     what the attack test rolled, what an op is allowed to do.
//   EFFECT (skills.cpp, wasm later) -- what to ask for, given that.
// An effect never validates anything and never applies anything, so an
// untrusted one cannot do anything the engine did not already permit.

#pragma once

#include "badlands_sim.hpp"  // SkillId, SkillSpec
#include "skill_abi.h"       // BlSkillCastContext, BlSkillEffectBatch

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// Seed axis for a skill's own attack test, keeping its roll distinct from any
// weapon swing resolved in the same tick against the same target: combat_seed
// folds attack_index (combat.cpp), so a bash sharing index 0 with the sword it
// borrowed would share the sword's roll. Well clear of kMaxAttacks.
inline constexpr int32_t kSkillSeedBase = 100;

// The cast range: the reach of whichever attack the skill's SkillAttackTest
// names (Melee -> melee reach, Ranged -> ranged reach), or -- for a skill that
// declares no test -- its optional "range" constant, whose absence means "no
// range check", which is what a SelfOnly ward wants. One source of truth: a
// bash reaches exactly as far as the sword whose test it borrows.
float skill_cast_range(const entt::registry& reg, entt::entity caster, const SkillSpec& spec);

// A validated cast, ready to run. Produced by validate_cast, consumed by the
// UseSkill command handler.
struct CastPlan {
    entt::entity caster = entt::null;
    SkillId id = SkillId::Count;
    const SkillSpec* spec = nullptr;
    uint32_t targets[kMaxSkillTargets]{};
    int32_t target_count = 0;
};

// Every check the engine makes before an effect may run, in one place so the
// early gateway refusal (resolve_action, intention.h) and the authoritative
// one (the UseSkill command handler, command.cpp) cannot drift apart. In
// order, each a warn + false:
//   1. `caster_slot` names a live entity carrying Skills.
//   2. `skill_index` is a live index into ITS loadout (not a SkillId -- the
//      same convention BL_ACT_ATTACK's attack index uses).
//   3. That skill is off cooldown.
//   4. Its trigger is one the engine executes (Action today; Passive and
//      Intention are declared vocabulary and refused here, never approximated).
//   5. Its targeting mode resolves against `named_target_slot`: None targets
//      nobody, SelfOnly targets the caster and REFUSES a named other, Any
//      takes the named entity. Multi/Point are declared and refused.
//   6. Every resolved target is within skill_cast_range.
bool validate_cast(const BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
                   uint32_t named_target_slot, CastPlan& out);

// Builds the flat context an effect receives: the caster's view, one view per
// resolved target (defensive stats via effective_combatant, so a stunned
// target reads as defenceless), the pre-rolled attack test per target, and the
// spec's constants. Pure over the registry -- same inputs, same context, live
// or replaying.
BlSkillCastContext build_cast_context(const BadlandsGame& game, entt::entity caster,
                                      SkillId id, const SkillSpec& spec,
                                      const uint32_t target_slots[], int32_t target_count);

// Applies what the effect asked for. Every op is re-checked against the
// context it was given: the target must be one of the context's own targets
// (an effect cannot reach anything the engine did not show it), the status
// kind must be in range, and durations/amounts are clamped to >= 0. An
// unrecognized op kind warns and is skipped rather than aborting the batch, so
// a newer effect's vocabulary degrades instead of crashing.
void apply_effect_batch(BadlandsGame& game, uint32_t caster_slot,
                        const BlSkillCastContext& ctx, const BlSkillEffectBatch& batch);

// The whole cast, from a validated plan: build the context, run the effect,
// apply the batch, stamp the cooldown, emit SkillUsed. The UseSkill command
// handler's body (command.cpp) -- kept here so the command layer stays a
// switch over one-line calls.
void run_cast(BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
              const CastPlan& plan);

}  // namespace badlands
