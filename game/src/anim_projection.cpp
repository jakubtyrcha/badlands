#include "anim_projection.h"

#include "components.h"
#include "game_state.h"
#include "status.h"  // has_status, remaining_ticks_of

#include <glm/glm.hpp>

#include <cmath>

namespace badlands {
namespace {

struct Resolved {
    AnimAction action = AnimAction::Idle;
    int64_t start_ticks = 0;
    // 0 means "this action has no end" (a loop). A stun supplies an end without
    // a start, which is why the two endpoints are tracked independently.
    int64_t end_ticks = 0;
    int32_t param = -1;
    // Whether the MECHANIC authored the start too. False means the start must be
    // edge-stamped: a loop has none, and a status records what is LEFT rather
    // than when it began.
    bool authored_start = false;
};

// Own-power distance moved this step -> speed. Reads the movement pass' own
// per-slot record rather than differencing positions, which is what makes a
// character that was REPOSITIONED (leaving a building, shoved apart by crowd
// separation) read 0 instead of spiking: those writes are not own-power motion
// and never enter this buffer.
float speed_of(const BadlandsGame& game, uint32_t slot) {
    if (slot >= game.moved_by_path_scratch.size()) return 0.0f;
    const float distance = glm::length(game.moved_by_path_scratch[slot]);
    if (!(distance > 0.0f)) return 0.0f;  // also rejects a NaN
    return distance / kSecondsPerStep;
}

// The priority order, highest first. Exactly one action describes a character,
// so the first match wins and the rest are not consulted.
Resolved resolve(const BadlandsGame& game, entt::entity e, float speed) {
    const entt::registry& reg = game.registry;
    const int64_t now = game.world_ticks;

    // A stun outranks everything. It cancels a wind-up outright (strike.h), and
    // during the recovery it cannot be cancelled but IS what the character is
    // visibly doing -- staggering, not following through.
    if (has_status(reg, e, StatusKind::Stunned)) {
        const int64_t remaining = remaining_ticks_of(reg, e, StatusKind::Stunned);
        // Only the END is known: a status records what is LEFT, not when it
        // began, so the start is edge-stamped by the caller.
        return Resolved{.action = AnimAction::Stunned,
                        .end_ticks = now + remaining,
                        .authored_start = false};
    }

    if (const auto* strike = reg.try_get<StrikeInProgress>(e); strike != nullptr) {
        // Phase is derived from the clock, exactly as strike.h derives it, so
        // there is no second copy of the rule to fall out of sync.
        const bool winding_up = now < strike->resolve_at_ticks;
        return Resolved{
            .action = winding_up ? AnimAction::AttackWindUp : AnimAction::AttackRecovery,
            .start_ticks = winding_up ? strike->declared_ticks : strike->resolve_at_ticks,
            .end_ticks = winding_up ? strike->resolve_at_ticks : strike->free_at_ticks,
            .param = strike->attack_index,
            .authored_start = true};
    }

    if (const auto* focus = reg.try_get<SkillFocus>(e); focus != nullptr) {
        return Resolved{.action = AnimAction::CastFocus,
                        .start_ticks = focus->declared_ticks,
                        .end_ticks = focus->resolve_at_ticks,
                        .param = static_cast<int32_t>(focus->id),
                        .authored_start = true};
    }

    if (speed > 0.0f) {
        return Resolved{.action = AnimAction::Locomotion};
    }
    return Resolved{.action = AnimAction::Idle};
}

}  // namespace

void project_anim_state(BadlandsGame& game) {
    entt::registry& reg = game.registry;

    // Slots ARE the characters (buildings have their own id space), and the slot
    // index is also the index into moved_by_path_scratch -- so this loop shape
    // is required, not incidental. Same iteration as characters_of (sim.cpp).
    for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
        const entt::entity e = game.slots[slot];
        if (!reg.valid(e)) continue;  // slots are never reused; dead leave null

        const float speed = speed_of(game, slot);
        const Resolved resolved = resolve(game, e, speed);

        CharacterAnim next;
        next.action = resolved.action;
        next.action_param = resolved.param;
        next.speed = speed;

        if (resolved.authored_start) {
            // The mechanic's own window is authoritative, so two back-to-back
            // swings differ by construction and the edge detector needs no help.
            next.action_start_ticks = resolved.start_ticks;
            next.action_end_ticks = resolved.end_ticks;
        } else {
            // An unbounded action has no authored start, so stamp one the tick
            // its KIND changes. Without this every loop would report the same
            // start forever and an observer could not tell a new one from a
            // continuing one.
            const auto* previous = reg.try_get<CharacterAnim>(e);
            const bool changed = previous == nullptr ||
                                 previous->action != resolved.action ||
                                 previous->action_param != resolved.param;
            next.action_start_ticks =
                changed ? game.world_ticks : previous->action_start_ticks;
            // A stun knows its end but not its beginning, so keep the real end
            // and its progress stays expressible. A true loop has no end at all,
            // which is encoded as end == start.
            next.action_end_ticks = resolved.end_ticks > next.action_start_ticks
                                        ? resolved.end_ticks
                                        : next.action_start_ticks;
        }

        reg.emplace_or_replace<CharacterAnim>(e, next);
    }
}

}  // namespace badlands
