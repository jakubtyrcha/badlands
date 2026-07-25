#include "intention.h"

#include "badlands_sim.hpp"  // BuildingKind
#include "brain_abi.h"       // BL_ACT_*
#include "combat.h"          // attack_usable, select_target
#include "command.h"         // CommandKind, Command, enqueue_move_to, enqueue_set_behavior
#include "components.h"
#include "game_state.h"      // BadlandsGame, entity_for_slot, slot_for_entity
#include "movement.h"        // kArriveRadius
#include "placement.h"       // nearest_building_of, PlacementState

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

void push_inbox_event(BadlandsGame& game, entt::entity e, InboxEvent ev) {
    auto* inbox = game.registry.try_get<EventInbox>(e);
    if (inbox == nullptr) {
        return;  // non-heroes carry no inbox -- a silent no-op, not an error
    }
    ev.at_millis = game.world_millis;
    ev.ttl_millis = kInboxTtlMillis;
    // Wake bookkeeping (Fix 1, EventInbox's own comment): bump the
    // timestamp-free sequence counter on EVERY push, regardless of eviction
    // below -- should_wake's event clause only cares whether something
    // landed since the last think, not which slot it ended up in.
    ++inbox->last_pushed_seq;
    if (inbox->count < kInboxCapacity) {
        inbox->events[inbox->count++] = ev;
        return;
    }
    // Full: newest evicts oldest. events[] is kept oldest-first, so dropping
    // index 0 and appending at the end is the whole ring.
    for (int32_t i = 1; i < kInboxCapacity; ++i) {
        inbox->events[i - 1] = inbox->events[i];
    }
    inbox->events[kInboxCapacity - 1] = ev;
}

bool apply_intention(BadlandsGame& game, uint32_t slot, const Intention& intent) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<CurrentIntention>(e)) {
        return false;  // no hero at this slot to adopt anything
    }
    entt::registry& reg = game.registry;
    const glm::vec2 self_pos = reg.get<Position>(e).pos;

    // Fetched once here, reused by the tail below. The wake-bookkeeping
    // stamps that USED to live here unconditionally (last_think_millis,
    // the rejection backoff) moved to note_think_outcome (intention.h) --
    // this function is purely validate-and-adopt now; see both doc
    // comments for why they are split.
    CurrentIntention& ci = reg.get<CurrentIntention>(e);

    switch (intent.kind) {
        case IntentionKind::None:
            return false;  // nothing suggested this wake

        case IntentionKind::MoveTo:
            if (!std::isfinite(intent.point.x) || !std::isfinite(intent.point.y)) {
                spdlog::warn("[intention] slot {}: MoveTo to a non-finite point, ignored", slot);
                return false;
            }
            enqueue_move_to(game, slot, intent.point);
            break;

        case IntentionKind::Attack:
            // Actor-only: melee whatever this hero is already engaged with.
            // UINT32_MAX -> fire_attack picks the nearest enemy, same as
            // every other producer that names none. param_a = -1 (explicit,
            // not Command::param_a's own 0 default): this is the legacy
            // auto-pick path (fire_attack, combat.h), not a choice of a
            // specific attack index -- resolve_action (above) is the only
            // producer that ever names one.
            game.command_queue.push_back({CommandKind::Attack, slot, UINT32_MAX, {0.0f, 0.0f}, -1});
            break;

        case IntentionKind::Shoot: {
            if (entity_for_slot(game, static_cast<int32_t>(intent.target_slot)) == entt::null) {
                spdlog::warn("[intention] slot {}: Shoot at unknown target_slot {}", slot,
                             intent.target_slot);
                return false;
            }
            // param_a = -1: same auto-pick reasoning as the Attack case above.
            game.command_queue.push_back(
                {CommandKind::Attack, slot, intent.target_slot, {0.0f, 0.0f}, -1});
            break;
        }

        case IntentionKind::Enter: {
            // arg is a BuildingKind (docs/design/intention-contract.html's
            // vocab table); "enterable" means at least one building of that
            // kind exists somewhere -- whether the hero is close enough to
            // actually step through the door is EnterBuilding's own
            // authoritative re-check (heroes.cpp's hero_enter), same as every
            // other command handler.
            if (intent.arg < 0 || intent.arg >= static_cast<int32_t>(BuildingKind::Count) ||
                nearest_building_of(game.placement, intent.arg, self_pos) == UINT32_MAX) {
                spdlog::warn("[intention] slot {}: Enter names no enterable building (kind {})",
                             slot, intent.arg);
                return false;
            }
            game.command_queue.push_back(
                {CommandKind::EnterBuilding, slot, UINT32_MAX, {0.0f, 0.0f}, intent.arg});
            break;
        }

        case IntentionKind::EnterHome: {
            const auto* sim = reg.try_get<HeroSimulationState>(e);
            const int32_t home = (sim != nullptr) ? sim->home_building_id : -1;
            if (home < 0 || static_cast<size_t>(home) >= game.placement.buildings.size() ||
                !game.placement.buildings[home].alive) {
                spdlog::warn("[intention] slot {}: EnterHome with no live home building", slot);
                return false;
            }
            game.command_queue.push_back({CommandKind::EnterHome, slot});
            break;
        }

        case IntentionKind::Buy:
            // Nothing to name: hero_buy locates the nearest Apothecary itself
            // (heroes.cpp), authoritatively, at execution time.
            game.command_queue.push_back({CommandKind::Buy, slot});
            break;

        case IntentionKind::Chat: {
            entt::entity partner = entity_for_slot(game, static_cast<int32_t>(intent.target_slot));
            if (partner == entt::null || partner == e ||
                !reg.all_of<HeroSimulationState>(partner)) {
                spdlog::warn("[intention] slot {}: Chat with unknown/non-hero target_slot {}",
                             slot, intent.target_slot);
                return false;
            }
            game.command_queue.push_back({CommandKind::Chat, slot, intent.target_slot});
            break;
        }

        case IntentionKind::Idle:
            if (intent.duration_millis < 0) {
                spdlog::warn("[intention] slot {}: Idle with a negative duration, ignored", slot);
                return false;
            }
            // Review fix: adopting Idle must HALT movement, not merely stop
            // issuing new orders -- MoveTarget/NavPath are durable state
            // nothing else clears, so a hero mid-MoveTo that gets told to
            // idle would otherwise keep walking toward the OLD goal
            // forever. v1's act_idle held position the same way; mirror it
            // here via the ordinary MoveTo producer (self position), so the
            // hold is itself a logged, replayable Command like every other
            // adopted intention, not a side effect apply_intention performs
            // outside the command log.
            enqueue_move_to(game, slot, self_pos);
            break;  // the SetBehavior below carries duration/wake scheduling
    }

    // Adopted. Idle's own duration IS its wake_at (and its completion
    // criterion, advance_intentions); every other kind gets the idle-hint as
    // a spurious-wake reminder only. Logged via enqueue_set_behavior's
    // duration field, so the wake schedule is IN the command log (the
    // SetBehavior handler, command.cpp, derives CurrentIntention::
    // wake_at_millis from it too, so a replay reconstructs the schedule from
    // the log alone -- see docs/design/intention-contract.html §6.
    // `force=true`: every ADOPTED intention is a real
    // decision (a wake, gated sparsely by should_wake) and must reach the
    // log even when activity_label happens to repeat the previous wake's --
    // enqueue_set_behavior's ordinary edge-trigger exists to dedupe a
    // PER-TICK re-decision (the mock brain's own use), which does not apply
    // here.
    //
    // Idle duration_millis == 0 is an EXPLICIT choice, not an oversight:
    // wake_at_millis == 0 is the "no deadline" sentinel (see
    // CurrentIntention's doc comment), so an Idle stamped with duration 0
    // reads as "idle until woken" -- it never self-completes and never
    // spuriously wakes on a deadline -- rather than "already expired." That
    // is why every deadline check in this file tests `> 0`, never `>= 0`,
    // and why Idle rejects only a NEGATIVE duration below, not a zero one.
    const int64_t deadline =
        (intent.kind == IntentionKind::Idle) ? intent.duration_millis : intent.idle_hint_millis;
    enqueue_set_behavior(game, slot, intent.activity_label, deadline, /*force=*/true);

    ci.kind = intent.kind;
    // Stamp only the field(s) this KIND actually uses (docs/design/
    // intention-contract.html's vocab table) and zero the rest, so
    // advance_intentions can dispatch on ci.kind alone rather than inferring
    // "this intention has a target" from whether target_slot/arg/point
    // happen to be non-default. Without this, a future caller that populates
    // e.g. target_slot alongside a MoveTo (carried over from a prior
    // suggestion, say) would silently make advance_intentions treat it as a
    // target-bearing intention it is not.
    ci.point = (intent.kind == IntentionKind::MoveTo) ? intent.point : glm::vec2{0.0f, 0.0f};
    ci.target_slot = (intent.kind == IntentionKind::Shoot || intent.kind == IntentionKind::Chat)
                         ? intent.target_slot
                         : UINT32_MAX;
    ci.arg = (intent.kind == IntentionKind::Enter) ? intent.arg : 0;
    ci.started_at_millis = game.world_millis;
    ci.wake_at_millis = deadline > 0 ? game.world_millis + deadline : 0;
    return true;
}

bool resolve_action(BadlandsGame& game, uint32_t slot, const AgentAction& action) {
    if (action.kind != BL_ACT_ATTACK) {
        // BL_ACT_NONE, the reserved USE_SKILL/USE_POTION, and anything
        // unrecognized all land here -- forward-compat posture, same as
        // decode_suggestion's unknown-intention-kind case (wasm_brain.cpp).
        spdlog::warn("[action] slot {}: kind {} is reserved or unrecognized, dropped", slot,
                     action.kind);
        return false;
    }

    entt::registry& reg = game.registry;
    entt::entity actor = entity_for_slot(game, static_cast<int32_t>(slot));
    const Attacks* atk = (actor != entt::null) ? reg.try_get<Attacks>(actor) : nullptr;
    if (atk == nullptr || !reg.all_of<Position>(actor)) {
        spdlog::warn("[action] slot {}: BL_ACT_ATTACK from a slot with no live/armed actor, "
                     "dropped", slot);
        return false;
    }
    if (action.arg < 0 || action.arg >= atk->count) {
        spdlog::warn("[action] slot {}: BL_ACT_ATTACK index {} out of range (count {}), dropped",
                     slot, action.arg, atk->count);
        return false;
    }

    // Target resolution: a named slot must be a live entity; UINT32_MAX only
    // resolves while the actor is ALREADY mid-Attack (melee whatever it's
    // engaged with -- the same actor-only contract apply_intention's own
    // Attack case documents, above). A live select_target scan, not the
    // per-tick nearest_enemy_scratch cache (game_state.h) -- this can be
    // called from a context that cache was never populated for (a direct
    // test, or a future simple brain outside sim.cpp's think loop).
    entt::entity target = entt::null;
    uint32_t resolved_target_slot = UINT32_MAX;
    if (action.target_slot != UINT32_MAX) {
        target = entity_for_slot(game, static_cast<int32_t>(action.target_slot));
        resolved_target_slot = action.target_slot;
    } else {
        const auto* ci = reg.try_get<CurrentIntention>(actor);
        if (ci == nullptr || ci->kind != IntentionKind::Attack) {
            spdlog::warn("[action] slot {}: BL_ACT_ATTACK names no target and has no running "
                         "Attack intention to infer one from, dropped", slot);
            return false;
        }
        target = select_target(game, actor);
        resolved_target_slot = slot_for_entity(game, target);
    }
    if (target == entt::null) {
        spdlog::warn("[action] slot {}: BL_ACT_ATTACK target_slot {} names no live entity, "
                     "dropped", slot, action.target_slot);
        return false;
    }

    const float dist =
        glm::distance(reg.get<Position>(actor).pos, reg.get<Position>(target).pos);
    if (!attack_usable(*atk, action.arg, dist, reg.all_of<MeleeLock>(actor))) {
        spdlog::warn("[action] slot {}: attack {} not usable right now (cooldown/range/lock), "
                     "dropped", slot, action.arg);
        return false;
    }

    game.command_queue.push_back(
        {CommandKind::Attack, slot, resolved_target_slot, {0.0f, 0.0f}, action.arg});
    return true;
}

void advance_intentions(BadlandsGame& game) {
    entt::registry& reg = game.registry;

    // TTL housekeeping first, for every hero, regardless of whether it has a
    // running intention: sticky, time-based expiry (not read-based) is the
    // whole point of the inbox (docs/design/intention-contract.html §4).
    for (auto [e, inbox] : reg.view<EventInbox>().each()) {
        int32_t kept = 0;
        for (int32_t i = 0; i < inbox.count; ++i) {
            InboxEvent ev = inbox.events[i];
            ev.ttl_millis -= kMillisPerTick;
            if (ev.ttl_millis > 0) {
                inbox.events[kept++] = ev;
            }
        }
        inbox.count = kept;
    }

    // Completion/abort detection, v2: MoveTo completes on arrival, Idle
    // completes when its deadline passes. Attack/Buy have no completion
    // criterion of their own -- they simply run until a fresh wake (idle-hint
    // expiry, an inbox event) replaces them with the next adopted suggestion;
    // a real per-intention lifecycle (its own guaranteed-wake completion
    // event) is queued for the skills slice, not this one. Shoot/Chat abort
    // when their named target dies or is gone; Enter/EnterHome abort when
    // the building they named is no longer there. Dispatches on ci.kind
    // EXPLICITLY (a switch, not "does ci.target_slot happen to be
    // UINT32_MAX") -- apply_intention stamps target_slot/arg/point only for
    // the kinds that use them (zeroed for everyone else), so kind is the one
    // honest signal for which check applies; inferring it from a field's
    // value would silently reclassify a kind whose field was populated for
    // an unrelated reason.
    for (auto [e, ci] : reg.view<CurrentIntention>().each()) {
        if (ci.kind == IntentionKind::None) {
            continue;
        }
        bool ended = false;
        bool completed = false;

        switch (ci.kind) {
            case IntentionKind::MoveTo:
                if (glm::distance(reg.get<Position>(e).pos, ci.point) <= kArriveRadius) {
                    ended = true;
                    completed = true;
                }
                break;

            case IntentionKind::Idle:
                if (ci.wake_at_millis > 0 && game.world_millis >= ci.wake_at_millis) {
                    ended = true;
                    completed = true;
                }
                break;

            case IntentionKind::Shoot: {
                entt::entity target = entity_for_slot(game, static_cast<int32_t>(ci.target_slot));
                const Health* hp = (target != entt::null) ? reg.try_get<Health>(target) : nullptr;
                if (target == entt::null || (hp != nullptr && hp->hp <= 0.0f)) {
                    ended = true;  // completed stays false -> aborted
                }
                break;
            }

            case IntentionKind::Chat: {
                // Review fix: Chat's lifecycle is driven by ChattingState
                // (heroes.cpp), not target liveness -- a dead/gone target_slot
                // would never even fire here for a session that never
                // started (command.cpp's Chat handler declines and calls
                // abort_intention itself in that case, below `ended` never
                // needing to catch it). ci.arg is the started marker
                // (unused by Chat otherwise, per apply_intention's own
                // comment): flips to 1 once ChattingState actually appears
                // on this hero, so a subsequent tick where it is GONE again
                // means the session ran its course -- expiry, drift, a
                // threat, or the partner leaving, all decided by
                // advance_chats, never by this function. Before arg reaches
                // 1 (still walking over, or the session hasn't landed yet
                // this tick), neither branch fires and the intention simply
                // keeps running, same as Attack/Buy.
                if (reg.all_of<ChattingState>(e)) {
                    ci.arg = 1;
                } else if (ci.arg == 1) {
                    ended = true;
                    completed = true;
                }
                break;
            }

            case IntentionKind::EnterHome: {
                const auto* sim = reg.try_get<HeroSimulationState>(e);
                const int32_t home = (sim != nullptr) ? sim->home_building_id : -1;
                if (home < 0 || static_cast<size_t>(home) >= game.placement.buildings.size() ||
                    !game.placement.buildings[home].alive) {
                    ended = true;
                }
                break;
            }

            case IntentionKind::Enter:
                if (ci.arg < 0 || ci.arg >= static_cast<int32_t>(BuildingKind::Count) ||
                    nearest_building_of(game.placement, ci.arg, reg.get<Position>(e).pos) ==
                        UINT32_MAX) {
                    ended = true;
                }
                break;

            case IntentionKind::Attack:
            case IntentionKind::Buy:
            case IntentionKind::None:
                break;  // no completion criterion of their own -- superseded by
                        // the next adopted suggestion (None already `continue`d above)
        }

        if (ended) {
            InboxEvent ev;
            ev.kind = InboxEventKind::IntentionEnded;
            ev.param = completed ? 1.0f : 0.0f;
            push_inbox_event(game, e, ev);

            ci.kind = IntentionKind::None;
            ci.target_slot = UINT32_MAX;
            ci.arg = 0;
            ci.wake_at_millis = 0;
        }
    }
}

bool should_wake(const BadlandsGame& game, entt::entity e) {
    if (!game.registry.valid(e)) {
        return false;
    }
    const auto* ci = game.registry.try_get<CurrentIntention>(e);
    // "Nothing running" is worth a wake only when NO backoff is armed
    // either (see this function's doc comment): note_think_outcome can set
    // wake_at_millis while leaving kind == None (a rejected/no-op
    // suggestion), and that hero must sleep out the backoff, not busy-wake
    // every tick -- so this can no longer be an unconditional short-circuit
    // the way it was before Fix 1.
    if (ci == nullptr || (ci->kind == IntentionKind::None && ci->wake_at_millis == 0)) {
        return true;  // fresh/never-consulted -- always worth a wake
    }
    if (ci->wake_at_millis > 0 && game.world_millis >= ci->wake_at_millis) {
        return true;  // Idle/idle-hint deadline, or a rejection backoff, elapsed
    }
    if (const auto* inbox = game.registry.try_get<EventInbox>(e)) {
        if (inbox->last_pushed_seq > inbox->last_seen_seq) {
            return true;  // something was pushed since the hero last thought
        }
    }
    return false;
}

void note_think_outcome(BadlandsGame& game, uint32_t slot, bool adopted) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    auto* ci = game.registry.try_get<CurrentIntention>(e);
    if (ci == nullptr) {
        return;
    }
    // Inspection only now -- should_wake reads the sequence counter below,
    // not this timestamp (see EventInbox's own comment on why).
    ci->last_think_millis = game.world_millis;
    if (auto* inbox = game.registry.try_get<EventInbox>(e)) {
        inbox->last_seen_seq = inbox->last_pushed_seq;
    }
    if (!adopted) {
        // Rejected or explicit "nothing new" (BL_INT_NONE): re-arm the
        // schedule so should_wake's deadline clause, not its "nothing
        // running" clause, governs the next wake -- see should_wake's own
        // comment for why the latter can no longer fire unconditionally.
        ci->wake_at_millis = game.world_millis + kRejectedSuggestionBackoffMillis;
    }
}

void abort_intention(BadlandsGame& game, uint32_t slot, IntentionKind expected) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    auto* ci = game.registry.try_get<CurrentIntention>(e);
    if (ci == nullptr || ci->kind != expected) {
        return;  // nothing to abort -- including a replayed world, where
                 // CurrentIntention.kind is always None (apply_intention
                 // never runs there)
    }
    ci->kind = IntentionKind::None;
    ci->target_slot = UINT32_MAX;
    ci->arg = 0;
    ci->wake_at_millis = 0;

    InboxEvent ev;
    ev.kind = InboxEventKind::IntentionEnded;
    ev.param = 0.0f;  // aborted
    push_inbox_event(game, e, ev);
}

}  // namespace badlands
