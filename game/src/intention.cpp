#include "intention.h"

#include "badlands_sim.hpp"  // BuildingKind
#include "brain_abi.h"       // BL_ACT_*
#include "combat.h"          // attack_usable, select_target, engagement_range
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

namespace {

// v3 restate-resume (docs/design/intention-contract.html §2, "Resume-by-
// default"): is `intent` indistinguishable from the intention already
// running in `ci`? Mirrors EXACTLY the per-kind field discipline
// apply_intention's own zeroing establishes below (point for MoveTo,
// target_slot for Shoot/Chat, arg for Enter) -- Attack/EnterHome/Buy carry no
// distinguishing field at all (the vocab table, docs/design/intention-
// contract.html §5, lists them actor-only/untargeted), so two suggestions of
// the same one of those kinds are always identical.
//
// Idle is DELIBERATELY excluded, always returning false: Idle's own "field"
// (duration_millis) is never stored on CurrentIntention -- only the deadline
// it produces (wake_at_millis) is, and that is a moving target (it is
// `now + duration`, so it differs across two calls even for the textually
// same duration). Treating "kind == Idle" alone as identical would silently
// swallow a genuinely new duration (e.g. a second Idle(900ms) landing on top
// of a still-running Idle(300ms)) as a no-op resume instead of the fresh
// decision it is -- exactly the case the wake-schedule replay test (§6)
// pins by using two different Idle durations back to back.
bool is_identical_restatement(const CurrentIntention& ci, const Intention& intent) {
    if (ci.kind != intent.kind) {
        return false;
    }
    switch (intent.kind) {
        case IntentionKind::MoveTo:
            return ci.point == intent.point;
        case IntentionKind::Shoot:
        case IntentionKind::Chat:
            return ci.target_slot == intent.target_slot;
        case IntentionKind::Enter:
            return ci.arg == intent.arg;
        case IntentionKind::Attack:
        case IntentionKind::EnterHome:
        case IntentionKind::Buy:
            return true;  // no distinguishing field -- actor-only/untargeted
        case IntentionKind::Idle:
        case IntentionKind::None:
        default:
            return false;  // never a restate -- see this function's own comment
    }
}

}  // namespace

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

    // Single-gateway combat's engagement executor (docs/superpowers/specs/
    // 2026-07-25-contract-v3-alignment-design.md §2): "fight whatever's
    // nearest" has no distinguishing field (is_identical_restatement, below,
    // always resumes an Attack restatement) so its chase-the-target half
    // cannot live inside the one-shot adopt path in the switch below -- it
    // has to refresh on EVERY call this wake suggests Attack, restate
    // included, which is why it runs here, ahead of the restate-resume
    // short-circuit. A LIVE select_target scan, not the per-tick
    // nearest_enemy_scratch cache (game_state.h): this runs inside the same
    // think pass the cache was built for (before apply_commands resolves
    // anything this tick), the same guarantee the deleted combat_preempt's
    // own cache read relied on, but paying for one extra live scan here
    // (only on an actual combat wake, should_wake-gated) is simpler than
    // threading the cache's hero-only-shaped guard through this seam too.
    // enqueue_engage (command.h) is a LOGGED Kind::Entity hold, not a
    // one-shot Kind::Point walk to a precomputed offset -- it tracks the
    // target's live position every plan_paths pass and stops at exactly
    // engagement_range, with no arrival-radius slop (see its own doc
    // comment for why that distinction matters).
    //
    // No swing here, ever: adoption/restatement is engagement-only. (V5
    // mandate: this case used to also push a same-tick auto-pick swing --
    // param_a = -1, bypassing resolve_action -- a live double-swing bug
    // whenever the target was already in range at the moment Attack was
    // first adopted. Deleted outright; swings come exclusively from
    // BL_ACT_ATTACK actions through resolve_action, below.) The
    // wake-INDEPENDENT abort (no living enemy anywhere) is
    // advance_intentions' job, below -- a hero/monster's next wake is not
    // guaranteed the instant its opponent dies (should_wake's high-stakes
    // clause dies with the same tick the ThreatSighted pass stops finding
    // one), so the engine must be able to notice and end the intention
    // without waiting for this function to run again.
    if (intent.kind == IntentionKind::Attack) {
        if (const entt::entity target = select_target(game, e); target != entt::null) {
            const Combatant& cb = reg.get<Combatant>(e);
            const Attacks& atk = reg.get<Attacks>(e);
            enqueue_engage(game, slot, slot_for_entity(game, target), engagement_range(cb, atk));
        }
        // target == null: nothing to chase this call -- advance_intentions'
        // Attack case will catch it (no living enemy -> IntentionEnded)
        // without this function needing to do anything more here.
    }

    // v3 restate-resume + restate-log dedup: an incoming suggestion identical
    // to what's already running is a fresh yield with a fresh hint, not a new
    // decision (the engine diffs) -- resume WITHOUT re-running the kind's
    // producer, without re-stamping started_at_millis, and WITHOUT logging
    // anything at all. Only the live `ci.wake_at_millis` refreshes from the
    // (defaulted) hint. This is command.h's own doctrine applied literally:
    // "re-stating an unchanged decision is not a decision" -- sameness is
    // implied by the ABSENCE of a log entry, exactly like every other
    // producer's ordinary edge-trigger (enqueue_move_to/enqueue_set_behavior)
    // already works. Idle can never land here (is_identical_restatement
    // excludes it), so `intent.idle_hint_millis` is always the right field to
    // read, never `intent.duration_millis`.
    //
    // Safety argument for dropping the force-log this path used to do:
    //   1. Replay never THINKS -- it never calls apply_intention at all
    //      (docs/design/intention-contract.html §6), so wake_at_millis can
    //      never influence anything replay actually reproduces (positions,
    //      hp, building occupancy -- all logged Commands). It is read-only
    //      input to should_wake, and should_wake only runs on a LIVE tick.
    //   2. A live run that continues from a replayed state can therefore only
    //      wake EARLY relative to what the original live run did: the log's
    //      last real schedule (the adoption, or the last CHANGED intention)
    //      is a floor every later identical restate's hint only pushes
    //      later, never earlier, so a replay stuck on that floor wakes no
    //      later than the original run would have. An early wake is a
    //      spurious wake, and this contract already treats those as free (an
    //      OFFER of consultation, not a promise of a new command,
    //      should_wake's own doc comment) -- and in combat specifically, the
    //      high-stakes clause forces a per-tick consult regardless of any
    //      deadline, so the "wrong" schedule is moot exactly when it would
    //      matter most.
    //   3. That makes this refresh LIVE-ONLY engine scheduling state (when to
    //      bother calling the brain again), never read back into anything the
    //      command log or replay observes -- the identical class
    //      note_think_outcome's own rejection-backoff already is (see that
    //      function's doc comment, intention.h, for the same determinism
    //      rationale stated once, not duplicated here).
    // Adoption (and any CHANGED intention, the tail below) still force-logs
    // its SetBehavior with the hint in param_b -- changes are logged,
    // sameness is implied by logging nothing.
    if (is_identical_restatement(ci, intent)) {
        const int64_t hint =
            intent.idle_hint_millis > 0 ? intent.idle_hint_millis : kDefaultWakeCadenceMillis;
        ci.wake_at_millis = game.world_millis + hint;
        return true;
    }

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
            // Engagement handled unconditionally above (it must run on a
            // restate too, so it cannot live only here). Adoption itself is
            // a pure CurrentIntention stamp -- no command of its own.
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
    // v3 supersedes the v2 rule that used to live in this comment (Idle
    // duration_millis == 0 reading as "idle until woken," wake_at_millis == 0
    // being the "no deadline" sentinel): docs/design/intention-contract.html
    // §2 now reads 0 as "no cadence preference," not "no deadline," for BOTH
    // fields -- kDefaultWakeCadenceMillis (components.h) is substituted for a
    // non-positive requested duration/hint either way, so `deadline` below is
    // now ALWAYS a genuine positive duration, never a sentinel. Idle still
    // rejects only a NEGATIVE duration above, not a zero one -- zero is valid
    // input, just no longer meaning "forever."
    const int64_t requested =
        (intent.kind == IntentionKind::Idle) ? intent.duration_millis : intent.idle_hint_millis;
    const int64_t deadline = requested > 0 ? requested : kDefaultWakeCadenceMillis;
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
    ci.wake_at_millis = game.world_millis + deadline;  // deadline is always > 0 now (see above)
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
        // No adoption re-check on this path (mandate carried from V4's
        // review): a named target_slot is accepted without confirming the
        // actor's CurrentIntention.kind is actually Attack right now. Inert
        // today, not unguarded by oversight -- every LIVE producer of a
        // named-target BL_ACT_ATTACK (hero.nim's bl_enqueue_action, this
        // brain's own monster_think) calls apply_intention(Attack) in the
        // SAME wake, before draining pending_actions/calling resolve_action,
        // and Attack currently has zero rejection paths (apply_intention's
        // Attack case always adopts) -- so by construction, ci->kind is
        // already Attack for the actor by the time either producer's named
        // action reaches here. If a future intention kind gains a rejection
        // path that a producer could race against its own action call, this
        // needs a real gate (mirroring the UINT32_MAX branch's own check,
        // below) -- flagging rather than adding an unreachable check now.
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
                // Wake-INDEPENDENT abort: Attack is untargeted (actor-only,
                // "fight whatever's nearest" -- apply_intention's own
                // comment above), so there is no single named target to
                // check for death/goneness the way Shoot does above; the
                // equivalent condition is "no living enemy exists anywhere
                // for this actor to fight" (select_target/nearest_enemy
                // returns null). Runs every tick regardless of whether a
                // wake happened this tick -- the engagement executor
                // (apply_intention's Attack case) only refreshes on an
                // actual wake, so a hero/monster whose last enemy died
                // between wakes would otherwise stay stuck reporting Attack
                // indefinitely. A live scan (select_target), for the same
                // post-movement-staleness reason apply_intention's own
                // Attack case gives (combat.cpp's select_target doc has the
                // fuller account of why every call site here needs one).
                if (select_target(game, e) == entt::null) {
                    ended = true;  // completed stays false -> aborted
                }
                break;

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
    // v3 high-stakes clause (docs/design/intention-contract.html §2, "Tiered
    // wake guarantees") -- checked FIRST, ahead of every other clause below,
    // so it wins even over a long idle-hint deadline that has not elapsed
    // yet: a hero told "sleep 4 hours" must still notice a monster that
    // walks up, and a hero locked in melee stays consultable every tick.
    // This is an ENGINE OFFER of per-tick consultation, not a promise that
    // every offered wake produces a new command -- a brain re-consulted
    // every tick while a threat lingers is expected to often just restate
    // what it was already doing, which apply_intention's restate-resume path
    // resumes without logging anything at all (restate-log dedup: the wake
    // schedule refresh is live-only now, see that path's own comment).
    //
    // threat_was_present reads THIS TICK's value, not an edge -- sim.cpp's
    // ThreatSighted pass (tick_world) runs BEFORE the think loop every tick
    // and maintains it there (force-resetting it false for a hidden hero, so
    // a hero currently hidden never reads true here), so by the time
    // should_wake is consulted this tick's perception has already landed;
    // "this tick's value" and "as of the last completed perception pass" are
    // the same read.
    if (const auto* inbox = game.registry.try_get<EventInbox>(e)) {
        if (inbox->threat_was_present) {
            return true;
        }
    }
    if (game.registry.all_of<MeleeLock>(e)) {
        return true;
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
