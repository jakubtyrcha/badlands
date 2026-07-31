#include "monster_brain.h"

#include "badlands_sim.hpp"
#include "brain_abi.h"  // BL_ACT_ATTACK
#include "combat.h"     // pick_attack, melee_range, ranged_range
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "intention.h"  // apply_intention, resolve_action, note_think_outcome
#include "placement.h"

#include <cmath>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

// How far outside a melee opponent's reach a ranged monster tries to stay.
// Mirrors hero.nim's kStandoffMargin -- the two brains make the same decision
// and should make it the same way.
constexpr float kSkirmishMargin = 1.5f;

// How far inside the margin the opponent must get before the retreat is
// re-issued. See the use site: without it, a held standoff writes one command
// per tick into the determinism trace.
constexpr float kSkirmishDeadBand = 0.5f;

// Nearest alive building a monster may attack (enemy_targettable: Castle/House),
// by distance to its approach tile ("door") -- the tile the rat walks to and
// bites from. Returns its id and door.
bool nearest_targettable(const BadlandsGame& game, glm::vec2 pos, uint32_t& out_id,
                         glm::vec2& out_door) {
    float best = 0.0f;
    bool found = false;
    const auto& bs = game.placement.buildings;
    for (uint32_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].alive ||
            !BuildingDefOf(static_cast<BuildingKind>(bs[i].kind)).enemy_targettable) {
            continue;
        }
        glm::vec2 door;
        if (!building_approach_tile(game.placement, bs[i], door)) {
            continue;
        }
        const float d = glm::distance(pos, door);
        if (!found || d < best) {
            best = d;
            out_id = i;
            out_door = door;
            found = true;
        }
    }
    return found;
}

}  // namespace

// The simple brain (docs/superpowers/specs/2026-07-25-contract-v3-alignment-
// design.md §2, "Simple brains"): the tier split is where a brain runs (wasm
// contract module vs. engine code), never which door it uses -- this brain
// issues its intention/action through the EXACT SAME seams a wasm hero uses
// (apply_intention / resolve_action, game/src/intention.h), not a privileged
// combat path. Consulted every tick (engine code is cheap, unlike a wasm
// hop), never should_wake-gated.
void monster_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    entt::registry& reg = game.registry;
    const glm::vec2 pos = reg.get<Position>(e).pos;

    // Same-tick cache shortcut (game_state.h's nearest_enemy_scratch doc
    // comment has the full account): this call runs inside the SAME think
    // pass sim.cpp's ThreatSighted pass populated the cache in, before
    // apply_commands resolves anything, so nothing has moved or died in
    // between -- the exact guarantee the deleted combat_preempt's own cache
    // read relied on. Guarded on EventInbox + bounds the same way that
    // read was; heroes.cpp's spawn recipe now emplaces EventInbox for
    // Archetype::Monster too, so this entity was always written by the
    // pass (never a never-populated slot).
    const bool cached = reg.all_of<EventInbox>(e) && slot < game.nearest_enemy_scratch.size();
    const entt::entity target =
        cached ? game.nearest_enemy_scratch[slot] : nearest_enemy(game, e);

    if (target != entt::null) {
        // Fight: restate the Attack intention -- apply_intention's own
        // Attack case (intention.cpp) is engagement-only (it maintains the
        // approach MoveTarget toward whichever enemy select_target/
        // nearest_enemy finds live; no swing there) -- then swing through
        // the SAME action gateway a wasm brain's bl_enqueue_action drains
        // into (resolve_action, game/src/intention.h), validated there at
        // resolve time exactly like a wasm hero's own pick. pick_attack is
        // this brain's OWN policy helper now (combat.h's own doc comment on
        // select_attack/pick_attack), not a host auto-pick on this actor's
        // behalf -- the same function hero.nim's pickBestAttack reimplements
        // guest-side, just called host-side here since this brain IS host
        // code.
        const bool adopted = apply_intention(
            game, slot,
            Intention{.kind = IntentionKind::Attack,
                      .activity_label = static_cast<int32_t>(ActivityId::Combat)});
        note_think_outcome(game, slot, adopted);

        const Attacks& atk = reg.get<Attacks>(e);
        const float dist = glm::distance(pos, reg.get<Position>(target).pos);

        // Skirmish (v5): the enemy-side mirror of hero.nim's standoff. Same
        // decision, same seams -- the tier split is where a brain RUNS (engine
        // code vs a wasm module), never which door it uses. A ranged monster
        // that outranges what is closing on it backs off to keep the margin
        // while still shooting through the action channel below; the shot's
        // own wind-up (game/src/strike.h) is what makes that cost ground.
        //
        // Never breaks contact deliberately: once melee-locked, walking out
        // would cost StatusKind::Disengaged, which is always worse than
        // fighting. Spacing is the whole job.
        const float own_ranged = ranged_range(atk);
        if (!reg.all_of<MeleeLock>(e) && own_ranged > 0.0f) {
            const Attacks* tatk = reg.try_get<Attacks>(target);
            const float their_reach = tatk != nullptr ? melee_range(*tatk) : 0.0f;
            const float their_ranged = tatk != nullptr ? ranged_range(*tatk) : 0.0f;
            const float want = their_reach + kSkirmishMargin;
            // A DEAD BAND, not `dist < want`. The retreat goal is derived from
            // the opponent's live position, so it drifts every tick -- and
            // enqueue_move_to can only dedupe a near-identical point. Without
            // the band a standoff appends a Point command per tick to
            // command_log, which is the determinism trace and the replay input,
            // and is exactly what that dedupe exists to prevent. Re-issuing
            // only once the margin has actually been eaten into keeps the
            // behaviour and drops the churn.
            if (own_ranged > their_reach && their_ranged <= 0.0f &&
                dist < want - kSkirmishDeadBand) {
                const glm::vec2 away = pos - reg.get<Position>(target).pos;
                const float len = glm::length(away);
                if (len > 1e-4f) {
                    enqueue_move_to(game, slot, pos + away / len * (want - dist));
                }
            }
        }

        const int pick = pick_attack(atk, dist, reg.all_of<MeleeLock>(e));
        if (pick >= 0) {
            resolve_action(game, slot,
                           AgentAction{BL_ACT_ATTACK, slot_for_entity(game, target), pick});
        }
        return;
    }

    // No unit target: the existing building-gnaw path, unchanged.
    uint32_t bid = UINT32_MAX;
    glm::vec2 door{};
    if (!nearest_targettable(game, pos, bid, door)) {
        reg.get<MoveTarget>(e).kind = MoveTarget::Kind::None;
        return;  // nothing to gnaw
    }

    // Walk to the door; the shared movement pipeline gets us there.
    enqueue_move_to(game, slot, door);

    // At the door and off cooldown -> swing (command-sourced, so building combat
    // is logged/replayable). Gated on arrival + cooldown, so the log gets one
    // entry per actual hit, not one per tick of the approach.
    const auto& stats = reg.get<Stats>(e);
    const auto& attacks = reg.get<Attacks>(e);
    const bool at_door = glm::distance(pos, door) <= stats.attack_range + kEntranceRadius;
    const bool ready = attacks.count > 0 && attacks.cooldown_remaining[0] <= 0.0f;
    if (at_door && ready) {
        game.command_queue.push_back({CommandKind::AttackBuilding, slot, bid});
    }
}

}  // namespace badlands
