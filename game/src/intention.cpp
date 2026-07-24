#include "intention.h"

#include "badlands_sim.hpp"  // BuildingKind
#include "command.h"         // CommandKind, Command, enqueue_move_to, enqueue_set_behavior
#include "components.h"
#include "game_state.h"      // BadlandsGame, entity_for_slot
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

    // Mark the think UNCONDITIONALLY, before any validation/adoption below --
    // even a rejected suggestion or an explicit IntentionKind::None means the
    // brain just looked at this hero's inbox, so should_wake (docs/design/
    // intention-contract.html §2) must not treat those same,
    // already-considered events as a fresh reason to wake it again next tick
    // (see should_wake's own comment). Fetched once here and reused by the
    // tail below.
    CurrentIntention& ci = reg.get<CurrentIntention>(e);
    ci.last_think_millis = game.world_millis;

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
            // every other producer that names none.
            game.command_queue.push_back({CommandKind::Attack, slot, UINT32_MAX});
            break;

        case IntentionKind::Shoot: {
            if (entity_for_slot(game, static_cast<int32_t>(intent.target_slot)) == entt::null) {
                spdlog::warn("[intention] slot {}: Shoot at unknown target_slot {}", slot,
                             intent.target_slot);
                return false;
            }
            game.command_queue.push_back({CommandKind::Attack, slot, intent.target_slot});
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
            break;  // no command producer of its own -- the SetBehavior below carries it
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

            case IntentionKind::Shoot:
            case IntentionKind::Chat: {
                entt::entity target = entity_for_slot(game, static_cast<int32_t>(ci.target_slot));
                const Health* hp = (target != entt::null) ? reg.try_get<Health>(target) : nullptr;
                if (target == entt::null || (hp != nullptr && hp->hp <= 0.0f)) {
                    ended = true;  // completed stays false -> aborted
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
    if (ci == nullptr || ci->kind == IntentionKind::None) {
        return true;  // nothing running -- always worth a wake
    }
    if (ci->wake_at_millis > 0 && game.world_millis >= ci->wake_at_millis) {
        return true;  // idle-hint / Idle deadline passed
    }
    if (const auto* inbox = game.registry.try_get<EventInbox>(e)) {
        for (int32_t i = 0; i < inbox->count; ++i) {
            // Strictly AFTER the last think, not `started_at_millis`/`>=`
            // (docs/design/intention-contract.html §2): events are written
            // BEFORE the think-dispatch loop within the SAME tick (sim.cpp's
            // tick_world), so an event that arrived the same tick a wake
            // already considered it would otherwise satisfy `>=` forever after
            // (`ci->last_think_millis` never advances again once the hero
            // goes back to sleep) -- an immediate, spurious re-wake next
            // tick for no NEW reason. last_think_millis is stamped on EVERY
            // apply_intention call, even a rejected suggestion or an
            // explicit "nothing new" (IntentionKind::None), so it always
            // reflects the last time this hero's inbox was actually looked
            // at, unlike started_at_millis (which only advances when an
            // intention is adopted).
            if (inbox->events[i].at_millis > ci->last_think_millis) {
                return true;  // something happened since the hero last thought
            }
        }
    }
    return false;
}

}  // namespace badlands
