// Intention contract engine groundwork (game/src/intention.h): the inbox
// ring, the engine-side writers (DamageTaken via emit_char_hit, MoveBlocked
// via movement.cpp, the edge-triggered ThreatSighted pass in sim.cpp's
// tick_world), the pure should_wake predicate, and apply_intention's
// validate-then-adopt seam. The contract is live: sim.cpp's think loop gates
// every wasm hero's tick on should_wake and adopts each wake's suggestion via
// apply_intention (wasm_brain.cpp) -- see docs/design/intention-contract.html
// for the contract these mirror.

#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"           // advance_chats
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

// --- Fix 4: a hidden hero must not carry a stale sighting edge -------------
// (finding: the pass EXCLUDED InsideBuilding heroes outright, so
// threat_was_present was left at whatever it was before the hero hid --
// stale `true` then silently suppresses the next real sighting after it
// re-emerges, since the edge-detector never sees the empty->nonempty
// transition).

TEST_CASE(
    "a hidden hero's sighting edge resets: the threat at exit reads as a "
    "fresh sighting",
    "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;

    CharacterDesc hero_d{};
    hero_d.hp = 20.0f;
    hero_d.team = 0;
    hero_d.size_x = hero_d.size_y = hero_d.size_z = 1.0f;
    entt::entity hero = g.slots[spawn_into(g, hero_d)];

    CharacterDesc monster_d{};
    monster_d.archetype = Archetype::Monster;
    monster_d.hp = 20.0f;
    monster_d.team = 1;
    monster_d.pos_x = 5.0f;  // within the default threat_radius (14)
    monster_d.size_x = monster_d.size_y = monster_d.size_z = 1.0f;
    entt::entity monster1 = g.slots[spawn_into(g, monster_d)];

    // Establish the edge: flag goes true, event written.
    tick_world(g, 1.0f / 30.0f);
    REQUIRE(g.registry.get<EventInbox>(hero).threat_was_present);

    // Hero hides. Fatigue floored first: should_leave_building (heroes.cpp)
    // releases a hero the instant its reserve reads full, and it starts
    // full (HeroSimulationState's spawn default) -- without this the hero
    // would pop back out on the very next advance_inside, before it ever
    // stayed hidden.
    g.registry.get<HeroSimulationState>(hero).fatigue = 0.0f;
    g.registry.emplace<InsideBuilding>(hero, /*building_id=*/0,
                                       static_cast<int32_t>(ActivityId::GoHome));
    tick_world(g, 1.0f / 30.0f);
    REQUIRE(g.registry.all_of<InsideBuilding>(hero));  // still hidden

    // The old threat dies and is destroyed while the hero is hidden.
    g.registry.get<Health>(monster1).hp = 0.0f;
    tick_world(g, 1.0f / 30.0f);
    REQUIRE(g.registry.all_of<InsideBuilding>(hero));  // still hidden

    // A new threat spawns while the hero is still hidden.
    CharacterDesc monster2_d = monster_d;
    monster2_d.pos_x = 6.0f;
    uint32_t monster2_slot = spawn_into(g, monster2_d);

    // Hero re-emerges.
    g.registry.remove<InsideBuilding>(hero);
    tick_world(g, 1.0f / 30.0f);

    const EventInbox& inbox = g.registry.get<EventInbox>(hero);
    bool found = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::ThreatSighted &&
            inbox.events[i].source_slot == monster2_slot) {
            found = true;
        }
    }
    CHECK(found);
}

// --- should_wake: the pure wake-rule truth table ----------------------------

TEST_CASE("should_wake: no intention, new event, deadline", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    // No CurrentIntention running (kind == None, the spawn default) -> always
    // worth a wake.
    CHECK(should_wake(g, e));

    // A running intention with an empty inbox and no deadline -> asleep.
    // note_think_outcome is the seq-contract equivalent of what
    // apply_intention used to stamp at adoption time (Fix 1: the two are
    // now split -- see both functions' doc comments).
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::MoveTo;
    ci.started_at_millis = g.world_millis;
    note_think_outcome(g, slot, /*adopted=*/true);
    CHECK_FALSE(should_wake(g, e));

    // An inbox event pushed AFTER the last think -> wake (Fix 1: compared by
    // sequence number, not timestamp, so this holds even within the SAME
    // tick -- see EventInbox's own comment on why a timestamp comparison
    // cannot distinguish a pre-think push from a post-think one).
    g.world_millis += kMillisPerTick;
    InboxEvent ev;
    ev.kind = InboxEventKind::DamageTaken;
    push_inbox_event(g, e, ev);
    CHECK(should_wake(g, e));
    note_think_outcome(g, slot, /*adopted=*/true);  // the hero looked -> asleep again
    CHECK_FALSE(should_wake(g, e));

    // wake_at deadline passed -> wake, even with an empty inbox. Advance the
    // clock first: wake_at_millis == 0 is the "none" sentinel, so a deadline
    // has to be a genuine positive timestamp to test the check at all.
    g.world_millis += 1000;
    ci.wake_at_millis = 500;  // already behind the clock
    CHECK(should_wake(g, e));
}

// --- Fix 1 RED A: a same-tick, POST-think inbox push must still count as
// "new" (finding: should_wake's event clause used strict `>` on
// TIMESTAMPS -- at_millis vs last_think_millis -- and every event pushed
// within the SAME tick shares one world_millis stamp, so a push that lands
// AFTER a think that already happened this tick is timestamp-indistinguishable
// from one that landed BEFORE it and already informed that think). Written
// against TODAY's fields (ci.last_think_millis, poked directly -- mirrors
// what apply_intention currently stamps) so it fails for the documented
// reason before the seq-based fix lands; see the fix's later revision of
// this same case once note_think_outcome exists.

TEST_CASE(
    "should_wake: an inbox event pushed AFTER a same-tick think still counts as new",
    "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    // A running intention that just thought THIS tick (last_think_millis ==
    // world_millis, exactly what apply_intention stamps at adoption time).
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = 0;  // no deadline -- isolate the inbox-event path
    ci.last_think_millis = g.world_millis;

    // Something pushes an event to this hero's inbox LATER in the SAME
    // tick (e.g. movement's MoveBlocked mirror, which runs after the
    // think-dispatch loop) -- push_inbox_event stamps at_millis from the
    // CURRENT world_millis, which has not advanced, so at_millis ==
    // last_think_millis even though this push is genuinely new information
    // the hero has not yet seen.
    InboxEvent ev;
    ev.kind = InboxEventKind::MoveBlocked;
    push_inbox_event(g, e, ev);

    CHECK(should_wake(g, e));
}

// --- Fix 1 RED B: a rejected/no-op suggestion must back off, not busy-wake
// every tick (finding: with no CurrentIntention running, should_wake's
// "nothing running -- always worth a wake" branch fires unconditionally,
// so a brain that keeps declining gets re-consulted at the full 30 Hz tick
// rate instead of settling into a bounded re-check window).

TEST_CASE(
    "note_think_outcome re-arms a backoff on a rejected/no-op suggestion, "
    "so should_wake does not busy-wake every tick",
    "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    entt::entity e = g.slots[slot];

    // A hero whose deadline is already behind the clock -- the shape a
    // just-expired Idle/idle-hint wake looks like right before the brain is
    // consulted and declines to suggest anything new.
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = g.world_millis;  // already due

    note_think_outcome(g, slot, /*adopted=*/false);

    // Re-armed into the future, not left at/behind "now" -- a rejected
    // suggestion must not refire the deadline every single tick.
    CHECK(ci.wake_at_millis > g.world_millis);
    CHECK(ci.wake_at_millis == g.world_millis + kRejectedSuggestionBackoffMillis);
    CHECK_FALSE(should_wake(g, e));
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

// --- Fix 2: adopting Idle must halt movement, not just stop issuing new
// orders (finding: MoveTarget/NavPath are durable and nothing else clears
// them, so a hero mid-MoveTo told to idle used to keep walking toward the
// OLD goal forever).

TEST_CASE("apply_intention Idle halts a hero that was mid-MoveTo", "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc d = MercenaryDesc(0.0f, 0.0f);
    d.move_speed = 6.0f;
    uint32_t slot = spawn_into(g, d);
    entt::entity e = g.slots[slot];

    Intention move;
    move.kind = IntentionKind::MoveTo;
    move.point = {100.0f, 0.0f};  // far away, so it never arrives mid-test
    CHECK(apply_intention(g, slot, move));
    apply_commands(g);

    // A few ticks of real movement so the hero is genuinely underway.
    for (int i = 0; i < 5; ++i) {
        plan_paths(g, 1.0f / 30.0f);
        follow_paths(g, 1.0f / 30.0f);
    }
    const glm::vec2 moving_pos = g.registry.get<Position>(e).pos;
    CHECK(moving_pos.x > 0.0f);  // it did advance toward {100, 0}

    Intention idle;
    idle.kind = IntentionKind::Idle;
    idle.duration_millis = 60'000;  // long -- irrelevant to this check
    CHECK(apply_intention(g, slot, idle));
    apply_commands(g);

    // One more tick: position must NOT keep advancing toward the old goal.
    plan_paths(g, 1.0f / 30.0f);
    follow_paths(g, 1.0f / 30.0f);
    const glm::vec2 after_idle = g.registry.get<Position>(e).pos;
    CHECK(glm::distance(moving_pos, after_idle) < 0.01f);
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

// --- Fix 3: Chat lifecycle closed (a dangling no-op'd Chat when the session
// never starts; the participants not halted while chatting; completion never
// detected) ------------------------------------------------------------------

TEST_CASE(
    "Chat lifecycle (a): a never-started session (partner already occupied) "
    "aborts the actor's intention",
    "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t a_slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    uint32_t b_slot = spawn_into(g, MercenaryDesc(1.0f, 0.0f));  // within chat_radius
    uint32_t c_slot = spawn_into(g, MercenaryDesc(5.0f, 0.0f));
    entt::entity a = g.slots[a_slot];
    entt::entity b = g.slots[b_slot];

    // B is already mid-conversation with C (emplaced directly -- isolates
    // the scenario from needing a full live session to set it up).
    g.registry.emplace<ChattingState>(b, c_slot, 30.0f);

    Intention intent;
    intent.kind = IntentionKind::Chat;
    intent.target_slot = b_slot;
    CHECK(apply_intention(g, a_slot, intent));
    CHECK(g.registry.get<CurrentIntention>(a).kind == IntentionKind::Chat);

    apply_commands(g);  // drains the Chat command -- declines: B already chatting

    CHECK(g.registry.get<CurrentIntention>(a).kind == IntentionKind::None);
    const EventInbox& inbox = g.registry.get<EventInbox>(a);
    bool found_aborted = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded && inbox.events[i].param == 0.0f) {
            found_aborted = true;
        }
    }
    CHECK(found_aborted);
}

TEST_CASE("Chat lifecycle (b): movement halts for both participants while ChattingState is present",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t a_slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    CharacterDesc bd = MercenaryDesc(1.0f, 0.0f);
    bd.move_speed = 6.0f;
    uint32_t b_slot = spawn_into(g, bd);
    entt::entity a = g.slots[a_slot];
    entt::entity b = g.slots[b_slot];

    // B was mid-MoveTo toward a distant point when the chat started.
    MoveTarget& mt = g.registry.get<MoveTarget>(b);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {50.0f, 0.0f};

    const float duration = g.factors.hero.chat_duration;
    g.registry.emplace<ChattingState>(a, b_slot, duration);
    g.registry.emplace<ChattingState>(b, a_slot, duration);

    const glm::vec2 start = g.registry.get<Position>(b).pos;
    const float dt = 1.0f / 30.0f;
    // Stop a few ticks short of the session's own natural expiry -- driving
    // plan_paths/follow_paths/advance_chats directly (not tick_world) so
    // this pins the movement-exclusion fix in isolation, not entangled with
    // a brain re-issuing MoveTo.
    const int ticks = static_cast<int>(duration / dt) - 5;
    for (int i = 0; i < ticks; ++i) {
        advance_chats(g, dt);
        plan_paths(g, dt);
        follow_paths(g, dt);
        REQUIRE(g.registry.all_of<ChattingState>(b));  // hasn't dissolved early via drift
    }
    CHECK(glm::distance(g.registry.get<Position>(b).pos, start) < 0.01f);

    // Let it run to its natural end and confirm it still dissolves normally.
    for (int i = 0; i < 10; ++i) {
        advance_chats(g, dt);
    }
    CHECK_FALSE(g.registry.all_of<ChattingState>(b));
}

TEST_CASE("Chat lifecycle (c): a full session completes normally -- IntentionEnded(completed)",
          "[intention]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t a_slot = spawn_into(g, MercenaryDesc(0.0f, 0.0f));
    uint32_t b_slot = spawn_into(g, MercenaryDesc(1.0f, 0.0f));
    entt::entity a = g.slots[a_slot];
    entt::entity b = g.slots[b_slot];

    CurrentIntention& ci = g.registry.get<CurrentIntention>(a);
    ci.kind = IntentionKind::Chat;
    ci.target_slot = b_slot;
    ci.started_at_millis = g.world_millis;

    const float duration = g.factors.hero.chat_duration;
    g.registry.emplace<ChattingState>(a, b_slot, duration);
    g.registry.emplace<ChattingState>(b, a_slot, duration);

    const float dt = 1.0f / 30.0f;
    // One pass with ChattingState present -> the started marker (ci.arg) is set.
    advance_intentions(g);
    CHECK(g.registry.get<CurrentIntention>(a).kind == IntentionKind::Chat);

    // Run the session out via the real dissolve path (advance_chats) plus
    // advance_intentions, same order tick_world uses.
    const int ticks = static_cast<int>(duration / dt) + 5;
    for (int i = 0; i < ticks; ++i) {
        advance_chats(g, dt);
        advance_intentions(g);
    }

    CHECK(g.registry.get<CurrentIntention>(a).kind == IntentionKind::None);
    const EventInbox& inbox = g.registry.get<EventInbox>(a);
    bool found_completed = false;
    for (int32_t i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded && inbox.events[i].param == 1.0f) {
            found_completed = true;
        }
    }
    CHECK(found_completed);
}

// --- should_wake's inbox check (docs/design/intention-contract.html §2)
// must not treat an event that already informed the last think as a fresh
// reason to wake.

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
    // under test, and note_think_outcome has a genuine baseline think to
    // compare new pushes against.
    CurrentIntention& ci = g.registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = 0;  // no deadline -- isolate the inbox-event path
    note_think_outcome(g, slot, /*adopted=*/true);

    // An event arrives on a LATER tick (mirrors sim.cpp's tick_world: the
    // ThreatSighted/DamageTaken/MoveBlocked writers run BEFORE the
    // think-dispatch loop, in the SAME tick a hero might wake and decide --
    // this is that later tick's event, strictly after the think above).
    g.world_millis += kMillisPerTick;
    InboxEvent ev;
    ev.kind = InboxEventKind::ThreatSighted;
    push_inbox_event(g, e, ev);
    CHECK(should_wake(g, e));  // the event is new -> worth a wake

    // The brain wakes THIS tick, having seen the event, and decides there is
    // nothing new to suggest (IntentionKind::None) -- still a real think:
    // apply_intention returns false (nothing adopted), and the caller
    // (tick_wasm_brain) calls note_think_outcome regardless of that outcome,
    // which is what this line simulates.
    Intention nothing;  // kind defaults to IntentionKind::None
    CHECK_FALSE(apply_intention(g, slot, nothing));
    note_think_outcome(g, slot, /*adopted=*/false);

    // Next tick: the clock advances again; the SAME event is still sitting
    // in the (sticky, TTL-based) inbox -- but it already informed the
    // decision above. Without Fix 1 (last_pushed_seq vs last_seen_seq), this
    // would immediately re-wake the brain for no NEW reason. (The rejection
    // backoff note_think_outcome just armed does not interfere here either:
    // it is 500ms out, well past this one-tick advance.)
    g.world_millis += kMillisPerTick;
    CHECK_FALSE(should_wake(g, e));

    // A genuinely new event (after the think above) still wakes it.
    InboxEvent ev2;
    ev2.kind = InboxEventKind::DamageTaken;
    push_inbox_event(g, e, ev2);
    CHECK(should_wake(g, e));
}

// --- The wake schedule (docs/design/intention-contract.html §6) must be
// replay-derivable even when the activity label repeats across wakes.

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
    // edge-trigger case this test is about: enqueue_set_behavior's ordinary
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
