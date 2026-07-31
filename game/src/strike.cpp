#include "strike.h"

#include "combat.h"      // deliver_strike
#include "game_state.h"  // BadlandsGame, emit_event, slot_for_entity

#include <cmath>
#include <vector>

namespace badlands {

namespace {

// Seconds -> the integer millisecond clock. llround, not a truncation: a
// 0.35 s wind-up must be 350 ms on every platform, since the deadline is
// compared against a clock that advances by a fixed integer step.
int64_t millis_of(float seconds) {
    if (!(seconds > 0.0f)) {
        return 0;  // also catches NaN
    }
    return static_cast<int64_t>(std::llround(static_cast<double>(seconds) * 1000.0));
}

}  // namespace

bool declare_strike(BadlandsGame& game, entt::entity e, int32_t attack_index,
                    uint32_t target_slot) {
    entt::registry& reg = game.registry;
    if (e == entt::null || !reg.valid(e) || !reg.all_of<Attacks, Combatant>(e)) {
        return false;
    }
    if (reg.all_of<StrikeInProgress>(e)) {
        return false;  // one commitment at a time
    }
    const Attacks& atk = reg.get<Attacks>(e);
    if (attack_index < 0 || attack_index >= atk.count || attack_index >= kMaxAttacks) {
        return false;
    }

    StrikeInProgress s;
    s.declared_millis = game.world_millis;
    s.resolve_at_millis = game.world_millis + millis_of(atk.defs[attack_index].wind_up_seconds);
    s.free_at_millis =
        s.resolve_at_millis + millis_of(atk.defs[attack_index].recovery_seconds);
    s.attack_index = attack_index;
    s.target_slot = target_slot;
    s.attacker = reg.get<Combatant>(e);   // captured: the blow is what it was
    s.attack = atk.defs[attack_index];    // when it was thrown
    reg.emplace<StrikeInProgress>(e, s);
    return true;
}

bool striking(const entt::registry& reg, entt::entity e) {
    return e != entt::null && reg.valid(e) && reg.all_of<StrikeInProgress>(e);
}

bool winding_up(const entt::registry& reg, entt::entity e) {
    if (!striking(reg, e)) {
        return false;
    }
    // The caller has no clock, so this needs one -- but a StrikeInProgress that
    // has resolved is only ever observable BETWEEN advance_strikes calls, and
    // advance_strikes marks the transition by zeroing resolve_at. See its
    // comment: after the blow lands, resolve_at is set to INT64_MIN so "still
    // winding up" is a pure component read with no clock needed.
    return reg.get<StrikeInProgress>(e).resolve_at_millis != INT64_MIN;
}

bool cancel_strike(BadlandsGame& game, entt::entity e) {
    entt::registry& reg = game.registry;
    if (!winding_up(reg, e)) {
        return false;  // nothing committed, or already thrown -- not preventable
    }
    const StrikeInProgress s = reg.get<StrikeInProgress>(e);
    reg.erase<StrikeInProgress>(e);
    const glm::vec2 pos =
        reg.all_of<Position>(e) ? reg.get<Position>(e).pos : glm::vec2{0.0f, 0.0f};
    emit_event(game, GameEvent{.kind = GameEventKind::StrikeCancelled,
                               .actor_id = slot_for_entity(game, e),
                               .target_id = s.target_slot,
                               .target_kind = kEventTargetCharacter,
                               .amount = static_cast<float>(s.attack_index),
                               .x = pos.x,
                               .z = pos.y,
                               .at_millis = game.world_millis});
    return true;
}

void advance_strikes(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    // Collected first: deliver_strike spawns projectile entities and erase
    // invalidates the view, neither of which is safe mid-iteration.
    std::vector<entt::entity> to_land;
    for (auto [e, s] : reg.view<StrikeInProgress>().each()) {
        if (s.resolve_at_millis != INT64_MIN && game.world_millis >= s.resolve_at_millis) {
            to_land.push_back(e);
        }
    }

    for (entt::entity e : to_land) {
        StrikeInProgress& s = reg.get<StrikeInProgress>(e);
        // The cooldown is spent HERE, not at declaration: an interrupted
        // wind-up costs tempo but never the attack itself, and a landed one
        // starts recovering from the moment it landed.
        if (auto* atk = reg.try_get<Attacks>(e); atk != nullptr &&
                                                 s.attack_index >= 0 &&
                                                 s.attack_index < atk->count) {
            atk->cooldown_remaining[s.attack_index] = s.attack.cooldown;
        }
        const StrikeInProgress captured = s;
        // Mark it thrown BEFORE delivering: the blow can kill, and a lethal
        // hit's death sweep must not find a strike that still looks
        // cancellable. INT64_MIN is the sentinel winding_up reads, which keeps
        // "is this still preventable" a component read rather than a second
        // clock comparison every caller would have to get right.
        s.resolve_at_millis = INT64_MIN;
        deliver_strike(game, e, captured);
    }

    // Landing and freeing are ONE pass, not two ticks: an attack authored with
    // no wind-up and no recovery (every un-authored one, and every legacy test
    // fixture) must resolve within the tick it was declared in and leave no
    // residue, exactly as it did before commitment existed. This runs after
    // apply_commands (sim.cpp) for the same reason.
    std::vector<entt::entity> to_free;
    for (auto [e, s] : reg.view<StrikeInProgress>().each()) {
        if (s.resolve_at_millis == INT64_MIN && game.world_millis >= s.free_at_millis) {
            to_free.push_back(e);
        }
    }
    for (entt::entity e : to_free) {
        reg.erase<StrikeInProgress>(e);
    }
}

}  // namespace badlands
