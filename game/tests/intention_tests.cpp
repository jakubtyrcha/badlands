// Intention contract engine groundwork (game/src/intention.h): the inbox
// ring, the two engine-side writers wired this task (DamageTaken via
// emit_char_hit, MoveBlocked via movement.cpp), the edge-triggered
// ThreatSighted pass (sim.cpp's tick_world), the pure should_wake predicate,
// and apply_intention's validate-then-adopt seam. INERT this slice: nothing
// outside these tests calls apply_intention/should_wake yet, and the
// existing mock/wasm brain paths are untouched -- see docs/design/
// intention-contract.html for the contract these mirror.

#include "command.h"
#include "components.h"
#include "game_state.h"
#include "intention.h"
#include "movement.h"        // plan_paths, follow_paths -- drive the MoveBlocked mirror
#include "sim_internal.hpp"  // make_flat_world / spawn_into / tick_world

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

using namespace badlands;

// --- inbox: push / evict / TTL-expiry ---------------------------------------

TEST_CASE("push_inbox_event appends, evicts oldest when full, no-ops without an inbox",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    for (int i = 0; i < kInboxCapacity; ++i) {
        InboxEvent ev;
        ev.kind = InboxEventKind::DamageTaken;
        ev.param = static_cast<float>(i);
        push_inbox_event(g, e, ev);
    }
    const EventInbox& inbox = g.registry.get<EventInbox>(e);
    REQUIRE(inbox.count == kInboxCapacity);
    CHECK(inbox.events[0].param == 0.0f);
    CHECK(inbox.events[kInboxCapacity - 1].param == static_cast<float>(kInboxCapacity - 1));

    // One more push evicts the oldest (param 0) and lands at the end.
    InboxEvent overflow;
    overflow.kind = InboxEventKind::ThreatSighted;
    overflow.param = 99.0f;
    push_inbox_event(g, e, overflow);
    CHECK(inbox.count == kInboxCapacity);
    CHECK(inbox.events[0].param == 1.0f);  // param 0 evicted
    CHECK(inbox.events[kInboxCapacity - 1].param == 99.0f);

    // A non-hero (no EventInbox) silently ignores the push.
    CharacterDesc monster{};
    monster.archetype = Archetype::Monster;
    monster.hp = 10.0f;
    monster.pos_x = 20.0f;
    monster.size_x = monster.size_y = monster.size_z = 1.0f;
    entt::entity me = g.slots[spawn_into(g, monster)];
    REQUIRE_FALSE(g.registry.all_of<EventInbox>(me));
    push_inbox_event(g, me, overflow);  // must not crash
    CHECK_FALSE(g.registry.all_of<EventInbox>(me));
}

TEST_CASE("advance_intentions decrements inbox TTLs and drops expired entries", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    InboxEvent ev;
    ev.kind = InboxEventKind::DamageTaken;
    push_inbox_event(g, e, ev);  // ttl_millis = kInboxTtlMillis
    REQUIRE(g.registry.get<EventInbox>(e).count == 1);

    const int ticks_to_expire = static_cast<int>(kInboxTtlMillis / kMillisPerTick) + 1;
    for (int i = 0; i < ticks_to_expire; ++i) {
        g.world_millis += kMillisPerTick;
        advance_intentions(g);
    }
    CHECK(g.registry.get<EventInbox>(e).count == 0);
}

// --- DamageTaken (emit_char_hit) --------------------------------------------

TEST_CASE("emit_char_hit writes DamageTaken into the victim hero's inbox", "[intention]") {
    // Flat-world duel via direct spawns: no brain/tick needed, emit_char_hit
    // is the single choke point both real damage sites route through.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t victim_slot = spawn_into(g, MercenaryDesc(-2.0f, 0.0f));
    uint32_t attacker_slot = spawn_into(g, GoblinDesc(2.0f, 0.0f));
    entt::entity victim = g.slots[victim_slot];
    entt::entity attacker = g.slots[attacker_slot];

    const float hp0 = g.registry.get<Health>(victim).hp;
    emit_char_hit(g, attacker_slot, victim_slot, 6.0f, hp0 - 6.0f,
                  g.registry.get<Position>(victim).pos);

    const EventInbox& inbox = g.registry.get<EventInbox>(victim);
    REQUIRE(inbox.count == 1);
    CHECK(inbox.events[0].kind == InboxEventKind::DamageTaken);
    CHECK(inbox.events[0].source_slot == attacker_slot);
    CHECK(inbox.events[0].param == Catch::Approx(6.0f));

    // The attacker (a Goblin, Archetype::Monster) carries no inbox at all.
    CHECK_FALSE(g.registry.all_of<EventInbox>(attacker));
}

// --- MoveBlocked mirror ------------------------------------------------------

TEST_CASE("a refused step mirrors MoveBlocked into the hero's inbox", "[intention]") {
    auto owned = make_flat_world();  // terrain_blocking off -- use the arena wall instead
    BadlandsGame& g = *owned;
    g.arena_half_x = 5.0f;
    g.arena_half_z = 5.0f;

    CharacterDesc d = MercenaryDesc(4.0f, 0.0f);
    d.move_speed = 6.0f;
    entt::entity e = g.slots[spawn_into(g, d)];

    MoveTarget& mt = g.registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {100.0f, 0.0f};  // far past the east wall

    for (int i = 0; i < 60 && !g.registry.all_of<MoveBlocked>(e); ++i) {
        plan_paths(g, 1.0f / 30.0f);
        follow_paths(g, 1.0f / 30.0f);
    }
    REQUIRE(g.registry.all_of<MoveBlocked>(e));

    const EventInbox& inbox = g.registry.get<EventInbox>(e);
    bool found = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        found = found || inbox.events[i].kind == InboxEventKind::MoveBlocked;
    }
    CHECK(found);
}

// --- ThreatSighted: edge-triggered, not per-tick ----------------------------

TEST_CASE("tick_world writes ThreatSighted once on the empty->nonempty edge", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;

    // Both immobile (move_speed left at its zero-initialized default) and
    // harmless (attack_damage 0), so distance -- and therefore threat
    // presence -- stays constant across every tick; the only thing the edge
    // detector can see is the one true transition at spawn.
    CharacterDesc hero_d{};
    hero_d.hp = 20.0f;
    hero_d.team = 0;
    hero_d.size_x = hero_d.size_y = hero_d.size_z = 1.0f;
    entt::entity hero = g.slots[spawn_into(g, hero_d)];

    CharacterDesc monster_d{};
    monster_d.archetype = Archetype::Monster;
    monster_d.hp = 20.0f;
    monster_d.team = 1;
    monster_d.pos_x = 5.0f;  // well within the default threat_radius (14)
    monster_d.size_x = monster_d.size_y = monster_d.size_z = 1.0f;
    spawn_into(g, monster_d);

    for (int i = 0; i < 20; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }

    const EventInbox& inbox = g.registry.get<EventInbox>(hero);
    int32_t sightings = 0;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::ThreatSighted) {
            ++sightings;
        }
    }
    CHECK(sightings == 1);
}

// --- should_wake: the pure wake-rule truth table ----------------------------

TEST_CASE("should_wake: no intention, new event, deadline", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    entt::entity e = g.slots[spawn_into(g, MercenaryDesc(0.0f, 0.0f))];

    // No CurrentIntention running (kind == None, the spawn default) -> always
    // worth a wake.
    CHECK(should_wake(g, e));

    // A running intention with an empty inbox and no deadline -> asleep.
    // last_think_millis mirrors what apply_intention actually stamps at
    // adoption time (both to the SAME instant) -- see finding 1's fix.
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::MoveTo;
    ci.started_at_millis = g.world_millis;
    ci.last_think_millis = g.world_millis;
    ci.wake_at_millis = 0;
    CHECK_FALSE(should_wake(g, e));

    // An inbox event that arrives strictly AFTER the last think -> wake
    // (finding 1: the comparison is strict, not `>=`, so an event stamped at
    // the SAME instant as the last think -- one that already informed it --
    // must not by itself count as new; advance the clock first to test a
    // genuinely later event).
    g.world_millis += kMillisPerTick;
    EventInbox& inbox = g.registry.get<EventInbox>(e);
    InboxEvent ev;
    ev.kind = InboxEventKind::DamageTaken;
    ev.at_millis = g.world_millis;
    inbox.events[0] = ev;
    inbox.count = 1;
    CHECK(should_wake(g, e));
    inbox.count = 0;  // back to asleep for the next case
    CHECK_FALSE(should_wake(g, e));

    // wake_at deadline passed -> wake, even with an empty inbox. Advance the
    // clock first: wake_at_millis == 0 is the "none" sentinel, so a deadline
    // has to be a genuine positive timestamp to test the check at all.
    g.world_millis += 1000;
    ci.wake_at_millis = 500;  // already behind the clock
    CHECK(should_wake(g, e));
}

// --- apply_intention: validate + adopt --------------------------------------

TEST_CASE("apply_intention adopts a valid MoveTo, logs it, and carries the wake hint",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    Intention intent;
    intent.kind = IntentionKind::MoveTo;
    intent.point = {5.0f, 7.0f};
    intent.activity_label = static_cast<int32_t>(ActivityId::Explore);  // != spawn default (-1)
    intent.idle_hint_millis = 500;
    // Stray values a MoveTo never uses -- as if carried over from some prior
    // suggestion. Review fix: apply_intention must zero these for a kind
    // that doesn't use them, not stamp them onto CurrentIntention verbatim.
    intent.target_slot = 42;
    intent.arg = 7;

    CHECK(apply_intention(g, slot, intent));

    const CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    CHECK(ci.kind == IntentionKind::MoveTo);
    CHECK(ci.point.x == Catch::Approx(5.0f));
    CHECK(ci.point.y == Catch::Approx(7.0f));
    CHECK(ci.target_slot == UINT32_MAX);  // zeroed: MoveTo doesn't use a target
    CHECK(ci.arg == 0);                   // zeroed: MoveTo doesn't use arg
    CHECK(ci.started_at_millis == g.world_millis);
    CHECK(ci.wake_at_millis == g.world_millis + 500);

    apply_commands(g);  // drain the queue this task enqueued into

    bool found_move = false, found_setbehavior = false;
    for (const Command& c : g.command_log) {
        if (c.kind == CommandKind::MoveTo && c.actor == slot) {
            found_move = true;
            CHECK(c.point.x == Catch::Approx(5.0f));
            CHECK(c.point.y == Catch::Approx(7.0f));
        }
        if (c.kind == CommandKind::SetBehavior && c.actor == slot) {
            found_setbehavior = true;
            CHECK(c.param_a == static_cast<int32_t>(ActivityId::Explore));
            CHECK(c.param_b == 500);  // the idle hint rides the duration field
        }
    }
    CHECK(found_move);
    CHECK(found_setbehavior);
    CHECK(g.registry.get<MoveTarget>(e).point.x == Catch::Approx(5.0f));
}

TEST_CASE("apply_intention rejects Shoot at an unknown target and adopts nothing",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    // A prior, distinguishable CurrentIntention proves it is left untouched.
    CurrentIntention& ci_before = g.registry.get<CurrentIntention>(e);
    ci_before.kind = IntentionKind::Idle;
    ci_before.wake_at_millis = 12345;

    Intention intent;
    intent.kind = IntentionKind::Shoot;
    intent.target_slot = 999;  // never spawned

    CHECK_FALSE(apply_intention(g, slot, intent));

    const CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    CHECK(ci.kind == IntentionKind::Idle);
    CHECK(ci.wake_at_millis == 12345);
    CHECK(g.command_queue.empty());
}

TEST_CASE("apply_intention Idle stamps wake_at from the duration", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    Intention intent;
    intent.kind = IntentionKind::Idle;
    intent.duration_millis = 2000;

    CHECK(apply_intention(g, slot, intent));
    const CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    CHECK(ci.kind == IntentionKind::Idle);
    CHECK(ci.wake_at_millis == g.world_millis + 2000);
}

TEST_CASE("Idle duration 0 is \"idle until woken\": never self-completes, should_wake stays quiet",
          "[intention]") {
    // Rider fix: duration_millis == 0 collides with wake_at_millis == 0's
    // "no deadline" sentinel. The explicit choice is "idle until woken" (no
    // deadline at all), not "already expired" -- pin that both ways: the
    // intention itself never self-completes, and should_wake never fires on
    // a deadline it doesn't have.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    Intention intent;
    intent.kind = IntentionKind::Idle;
    intent.duration_millis = 0;
    CHECK(apply_intention(g, slot, intent));

    const CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    CHECK(ci.kind == IntentionKind::Idle);
    CHECK(ci.wake_at_millis == 0);  // not "already expired" -- "no deadline"

    for (int i = 0; i < 200; ++i) {
        g.world_millis += kMillisPerTick;
        advance_intentions(g);
        CHECK_FALSE(should_wake(g, e));  // no deadline, no event -> stays asleep
    }
    CHECK(ci.kind == IntentionKind::Idle);  // still running 200 ticks later
}

// --- advance_intentions: completion detection -------------------------------

TEST_CASE("advance_intentions completes a MoveTo on arrival and writes IntentionEnded",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::MoveTo;
    ci.point = g.registry.get<Position>(e).pos;  // already there
    ci.started_at_millis = g.world_millis;

    advance_intentions(g);

    CHECK(ci.kind == IntentionKind::None);  // completed -> cleared
    const EventInbox& inbox = g.registry.get<EventInbox>(e);
    bool found_completed = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded) {
            found_completed = found_completed || inbox.events[i].param == 1.0f;
        }
    }
    CHECK(found_completed);
}

TEST_CASE("advance_intentions aborts when a named target dies", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t hero_slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    uint32_t target_slot = spawn_into(g, GoblinDesc(2.0f, 0.0f));
    entt::entity hero = g.slots[hero_slot];
    entt::entity target = g.slots[target_slot];

    CurrentIntention& ci = g.registry.get<CurrentIntention>(hero);
    ci.kind = IntentionKind::Shoot;
    ci.target_slot = target_slot;
    ci.started_at_millis = g.world_millis;

    g.registry.get<Health>(target).hp = 0.0f;  // dies this tick
    advance_intentions(g);

    CHECK(ci.kind == IntentionKind::None);
    const EventInbox& inbox = g.registry.get<EventInbox>(hero);
    bool found_aborted = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded) {
            found_aborted = found_aborted || inbox.events[i].param == 0.0f;
        }
    }
    CHECK(found_aborted);
}

TEST_CASE("advance_intentions dispatches on ci.kind, not a stray target_slot", "[intention]") {
    // Review fix: apply_intention now zeroes target_slot for every kind but
    // Shoot/Chat, and advance_intentions switches on ci.kind explicitly --
    // this pins that a MoveTo with a target_slot some future caller left set
    // (bypassing apply_intention, as a direct registry poke here simulates)
    // still completes by arrival and is NOT reclassified as a target-bearing
    // intention just because the field happens to be non-default.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t hero_slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    uint32_t stray_slot = spawn_into(g, GoblinDesc(2.0f, 0.0f));
    entt::entity hero = g.slots[hero_slot];
    entt::entity stray = g.slots[stray_slot];

    CurrentIntention& ci = g.registry.get<CurrentIntention>(hero);
    ci.kind = IntentionKind::MoveTo;
    ci.point = g.registry.get<Position>(hero).pos;  // already there -> arrival
    ci.target_slot = stray_slot;                    // stray, unrelated to MoveTo
    ci.started_at_millis = g.world_millis;

    g.registry.get<Health>(stray).hp = 0.0f;  // the stray "target" dies this tick too
    advance_intentions(g);

    CHECK(ci.kind == IntentionKind::None);  // still ended (arrival), as expected
    const EventInbox& inbox = g.registry.get<EventInbox>(hero);
    bool found_completed = false, found_aborted = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded) {
            if (inbox.events[i].param == 1.0f) found_completed = true;
            if (inbox.events[i].param == 0.0f) found_aborted = true;
        }
    }
    CHECK(found_completed);      // arrival -> completed
    CHECK_FALSE(found_aborted);  // NOT aborted by the stray target's death
}

// --- Finding 1 (task-3-brief.md): should_wake's inbox check must not treat
// an event that already informed the last think as a fresh reason to wake.

TEST_CASE(
    "should_wake: an inbox event that informed the last think does not immediately re-wake "
    "next tick",
    "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    // A running intention that already thought once, so should_wake's
    // "nothing running" early return does not mask the inbox-event logic
    // under test, and last_think_millis has a genuine (non-default) baseline
    // to compare against.
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = 0;  // no deadline -- isolate the inbox-event path
    ci.last_think_millis = g.world_millis;

    // An event arrives on a LATER tick (mirrors sim.cpp's tick_world: the
    // ThreatSighted/DamageTaken/MoveBlocked writers run BEFORE the
    // think-dispatch loop, in the SAME tick a hero might wake and decide --
    // this is that later tick's event, strictly after the think above).
    g.world_millis += kMillisPerTick;
    EventInbox& inbox = g.registry.get<EventInbox>(e);
    InboxEvent ev;
    ev.kind = InboxEventKind::ThreatSighted;
    ev.at_millis = g.world_millis;
    inbox.events[0] = ev;
    inbox.count = 1;
    CHECK(should_wake(g, e));  // the event is new -> worth a wake

    // The brain wakes THIS tick, having seen the event, and decides there is
    // nothing new to suggest (IntentionKind::None) -- still a real think:
    // last_think_millis advances to now regardless of what was decided.
    Intention nothing;  // kind defaults to IntentionKind::None
    CHECK_FALSE(apply_intention(g, slot, nothing));

    // Next tick: the clock advances again; the SAME event is still sitting
    // in the (sticky, TTL-based) inbox -- but it already informed the
    // decision above. Without finding 1's fix (last_think_millis, compared
    // strictly), this would immediately re-wake the brain for no NEW reason.
    g.world_millis += kMillisPerTick;
    CHECK_FALSE(should_wake(g, e));

    // A genuinely new event (after the think above) still wakes it.
    InboxEvent ev2;
    ev2.kind = InboxEventKind::DamageTaken;
    ev2.at_millis = g.world_millis;
    inbox.events[inbox.count++] = ev2;
    CHECK(should_wake(g, e));
}

// --- Finding 2 (task-3-brief.md): the wake schedule must be replay-derivable
// even when the activity label repeats across wakes.

TEST_CASE(
    "the wake schedule replays: a repeated activity label still logs each wake's own duration",
    "[intention]") {
    auto live_owned = make_flat_world();
    BadlandsGame& live = *live_owned;
    uint32_t slot = spawn_into(live, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = live.slots[slot];

    Intention first;
    first.kind = IntentionKind::Idle;
    first.activity_label = static_cast<int32_t>(ActivityId::Idle);
    first.duration_millis = 300;
    live.world_millis = 1000;
    CHECK(apply_intention(live, slot, first));
    apply_commands(live);
    CHECK(live.registry.get<CurrentIntention>(e).wake_at_millis == 1300);

    // A SECOND wake, same activity_label (Idle) as `first` -- exactly the
    // edge-trigger case finding 2 is about: enqueue_set_behavior's ordinary
    // dedup would otherwise swallow this SetBehavior entirely, and the
    // second wake's own schedule would never reach the log at all.
    Intention second;
    second.kind = IntentionKind::Idle;
    second.activity_label = static_cast<int32_t>(ActivityId::Idle);
    second.duration_millis = 900;
    live.world_millis = 2000;
    CHECK(apply_intention(live, slot, second));
    apply_commands(live);
    CHECK(live.registry.get<CurrentIntention>(e).wake_at_millis == 2900);

    int32_t set_behavior_count = 0;
    for (const Command& c : live.command_log) {
        if (c.kind == CommandKind::SetBehavior && c.actor == slot) {
            ++set_behavior_count;
        }
    }
    REQUIRE(set_behavior_count == 2);

    // Replay: a fresh world, given an equivalent hero but NEVER calling
    // apply_intention/react() at all (sim.cpp's tick_world replay branch
    // never calls tick_wasm_brain/apply_intention -- see its own comment),
    // reconstructs the SAME wake_at_millis from the logged SetBehavior
    // commands alone, via apply_command's SetBehavior handler (command.cpp).
    auto replay_owned = make_flat_world();
    BadlandsGame& replay = *replay_owned;
    uint32_t replay_slot = spawn_into(replay, MercenaryDesc(0.0f, 0.0f));
    REQUIRE(replay_slot == slot);
    entt::entity re = replay.slots[replay_slot];

    replay.replay_log = &live.command_log;
    replay.world_millis = 1000;
    apply_replay_commands(replay);
    CHECK(replay.registry.get<CurrentIntention>(re).wake_at_millis == 1300);

    replay.world_millis = 2000;
    apply_replay_commands(replay);
    CHECK(replay.registry.get<CurrentIntention>(re).wake_at_millis == 2900);
}
