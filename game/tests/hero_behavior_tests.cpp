// Hero behavior, driven entirely by the shipping wasm brain
// (assets/brains/hero.wasm) -- the sole hero decision-maker now that the C++
// reference layer (town_think and friends) is gone. Histogram-in-spirit:
// these don't pin an exact tick-by-tick script the way the old block-level
// unit tests did (that level of detail now lives only in the Nim brain,
// scripts/brains/nim/), they pin OUTCOMES over a run -- a class does the
// thing it is for, policy (night, threats) shifts the mix the way it always
// did, and the world reacts to a hero the same way regardless of what is
// deciding for it.
//
// Also carries the two wasm-only behavioural smokes salvaged from the
// deleted hero_brain_parity_tests.cpp (its twin-brain-parity premise died
// with the C++ reference; these two cases needed no reference to begin
// with -- see git history at 1e6d867^ for the original).

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"
#include "sim_internal.hpp"

#include "fixtures/wasm_hero.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

using namespace badlands;

namespace {

uint32_t place_building(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    const int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t recruit_at(BadlandsGame& g, uint32_t building_id) {
    Action a{ActionKind::RecruitHero, building_id, 0.0f, 0.0f, 0, 0};
    const int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

// A durable deer to chase: HP high enough that it outlasts the whole window,
// so Hunt keeps having something to do across the full "a few hours" run
// instead of the count going quiet the moment it is shot dead.
uint32_t spawn_durable_deer(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 2;
    d.hp = 1.0e6f;
    d.move_speed = 3.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.7f;
    return spawn_into(g, d);
}

}  // namespace

// --- (1) a Hunter accrues Hunt entity-ticks over a few in-game hours -------

TEST_CASE("hero_behavior: a hunter accrues Hunt entity-ticks over a few in-game hours") {
    auto owned = testfix::make_wasm_world();
    BadlandsGame& g = *owned;

    // On the plains south of the central lake: the map origin is a lake, and
    // terrain blocking makes it impassable, so a chase staged there would
    // never close (hunter_tests.cpp's own convention).
    spawn_durable_deer(g, {6.0f, kCastleSpawnZ});
    const uint32_t hunter_slot = spawn_creature_into(g, CreatureId::Hunter, 0, {0.0f, kCastleSpawnZ});
    REQUIRE(hunter_slot != UINT32_MAX);

    ActivityHistogram hist;
    std::vector<CharacterState> rows;
    // ~4 in-game hours: long enough to cover several of the wasm brain's own
    // wake cycles (idle_hint_millis draws 0.5-2s, scripts/brains/nim/hero.nim),
    // not just a single lucky tick.
    const int kTicks = static_cast<int>(4 * millis_per_hour(g.millis_per_day) / kMillisPerTick);
    for (int i = 0; i < kTicks; ++i) {
        tick_world(g, 1.0f / 30.0f);
        characters_of(g, rows);
        hist.Accumulate(rows);
    }

    // A real threshold, not just ">0": a single fluke tick would pass a bare
    // nonzero check without proving the hunter actually spent the run hunting.
    CHECK(hist.ForClass(HERO_HUNTER, ActivityId::Hunt) > static_cast<uint64_t>(kTicks) / 4);
}

// --- (2) night raises a mildly-tired hero's GoHome share --------------------

TEST_CASE("hero_behavior: night raises a mildly-tired hero's GoHome share") {
    // Mirrors the deleted behaviours_tests.cpp's "at night the raised fatigue
    // bar sends a mildly-tired hero home" contract, at the sim level: the same
    // fatigue value reads as "fine, carry on" by day and "turn in" by night,
    // because the bar it is measured against moves, not because a tier does.
    constexpr int kTicks = 300;

    auto measure = [](bool night) {
        auto owned = testfix::make_wasm_world();
        BadlandsGame& g = *owned;
        const uint32_t guild = place_building(g, BuildingKind::FreeCompanyQuarters, -20.0f, 20.0f);
        REQUIRE(guild != UINT32_MAX);
        const uint32_t hid = recruit_at(g, guild);
        REQUIRE(hid != UINT32_MAX);
        const entt::entity e = g.slots[hid];

        // No apothecary/tavern in this fixture at all, so Buy/VisitTavern are
        // structurally inapplicable (has_apothecary/has_tavern false) -- the
        // only live contest is GoHome against the flat Roam fallback. Parked
        // right at the door: what is under test is the night/day bar, not
        // travel time.
        glm::vec2 home_door;
        REQUIRE(building_approach_tile(g.placement, g.placement.buildings[guild], home_door));
        g.registry.get<Position>(e).pos = home_door;
        g.world_millis = night ? static_cast<int64_t>(g.millis_per_day * 0.85) : (g.millis_per_day / 2);

        ActivityHistogram hist;
        std::vector<CharacterState> rows;
        for (int i = 0; i < kTicks; ++i) {
            // Hold fatigue/content steady tick to tick: what changes between
            // the two runs is ONLY night vs day, nothing drifting on its own.
            auto& sim = g.registry.get<HeroSimulationState>(e);
            sim.fatigue = 0.5f;  // below the night bar (0.90), above the day one (0.55)
            sim.content = 1.0f;
            tick_world(g, 1.0f / 30.0f);
            characters_of(g, rows);
            hist.Accumulate(rows);
        }
        REQUIRE(hist.Samples() == static_cast<uint64_t>(kTicks));
        return hist.Total(ActivityId::GoHome);
    };

    const uint64_t day_share = measure(false);
    const uint64_t night_share = measure(true);

    CHECK(day_share == 0);                     // fine by day: never once heads home
    CHECK(night_share > kTicks / 2);            // dominates once the bar moves
    CHECK(night_share > day_share);
}

// --- (3) a spawned rat flips idle heroes out of standing still -------------

TEST_CASE("hero_behavior: a spawned rat flips idle heroes into engaging it") {
    // Idle is the true last resort (activity_catalog.cpp: Roam's weight beats
    // it outright whenever both apply, which is always -- score_roam has no
    // veto), so getting a hero to genuinely settle on Idle takes taking Roam
    // out of contention for its class. What's left applicable at all (no
    // home/apothecary/tavern, no vision configured, no prey/partner) is Idle
    // alone.
    auto owned = testfix::make_wasm_world();
    BadlandsGame& g = *owned;

    SimFactors f = g.factors;
    f.hero.weights[HERO_MERCENARY].set(ActivityId::Roam, 0.0f);
    set_factors_of(g, f);

    std::vector<entt::entity> heroes;
    for (int i = 0; i < 2; ++i) {
        const uint32_t slot =
            spawn_into(g, MercenaryDesc(static_cast<float>(i) * 3.0f, kCastleSpawnZ));
        heroes.push_back(g.slots[slot]);
    }

    bool all_idle = false;
    for (int i = 0; i < 90 && !all_idle; ++i) {
        tick_world(g, 1.0f / 30.0f);
        all_idle = true;
        for (entt::entity e : heroes) {
            // Review fix (intention-contract fix wave): adopting Idle now
            // actively HOLDS position (a logged MoveTo to self, so a hero
            // mid-walk actually stops -- game/src/intention.cpp's Idle
            // branch), rather than leaving MoveTarget untouched. A holding
            // Idle hero therefore reads as Kind::Point AT its own position,
            // not Kind::None -- "not walking anywhere" is what this test
            // actually cares about, so accept either shape.
            const MoveTarget& mt = g.registry.get<MoveTarget>(e);
            const bool holding =
                mt.kind == MoveTarget::Kind::None ||
                (mt.kind == MoveTarget::Kind::Point &&
                 glm::distance(mt.point, g.registry.get<Position>(e).pos) < 0.5f);
            all_idle = all_idle &&
                      g.registry.get<HeroSimulationState>(e).behavior ==
                          static_cast<int32_t>(ActivityId::Idle) &&
                      holding;
        }
    }
    REQUIRE(all_idle);  // the premise: they really are idle before the rat shows up

    spawn_creature_into(g, CreatureId::Rat, /*team=*/1, {1.5f, kCastleSpawnZ - 5.0f});

    // Single-gateway combat (docs/superpowers/specs/2026-07-25-contract-v3-
    // alignment-design.md): engagement is now a BRAIN decision, not a
    // host-level pre-empt (combat_preempt is deleted) -- should_wake's own
    // high-stakes clause (threat_was_present/MeleeLock, intention.h) means
    // the wasm brain is consulted every tick a fight is on, restates
    // BL_INT_ATTACK, and the engine's engagement executor (apply_intention's
    // Attack case, intention.cpp) is what actually moves the hero (via
    // enqueue_engage, command.h). What visibly changes: CurrentIntention
    // flips to Attack, and MoveTarget stops holding position and starts
    // tracking the rat (Kind::Entity, not "none") -- both within a handful
    // of ticks of the rat coming into view, and an actual Attack command
    // follows once it closes the gap.
    bool engaged = false;
    for (int i = 0; i < 10 && !engaged; ++i) {
        tick_world(g, 1.0f / 30.0f);
        for (entt::entity e : heroes) {
            const bool attacking = g.registry.get<CurrentIntention>(e).kind == IntentionKind::Attack;
            const bool tracking = g.registry.get<MoveTarget>(e).kind == MoveTarget::Kind::Entity;
            engaged = engaged || (attacking && tracking);
        }
    }
    CHECK(engaged);

    bool attacked = false;
    for (int i = 0; i < 200 && !attacked; ++i) {
        tick_world(g, 1.0f / 30.0f);
        for (const Command& c : g.command_log) {
            attacked = attacked || c.kind == CommandKind::Attack;
        }
    }
    CHECK(attacked);
}

// --- salvaged from the deleted hero_brain_parity_tests.cpp (wasm-only smokes,
// no C++ reference involved to begin with -- see git history at 1e6d867^) ----

TEST_CASE("hero_behavior: wasm hero: an exhausted, homed hero decides GoHome") {
    auto owned = testfix::make_wasm_world();
    BadlandsGame& g = *owned;
    REQUIRE(g.wasm_brains != nullptr);

    const uint32_t guild = place_building(g, BuildingKind::FreeCompanyQuarters, -14.0f, -8.0f);
    REQUIRE(guild != UINT32_MAX);
    const uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);

    // Force exhaustion directly (needs_tests.cpp's own pattern) rather than
    // ticking out a real drain -- what is under test is "does GoHome fire
    // once fatigue crosses the bar", not how long that naturally takes.
    g.registry.get<HeroSimulationState>(g.slots[hid]).fatigue = 0.05f;

    bool went_home = false;
    for (int i = 0; i < 30 && !went_home; ++i) {
        tick_world(g, 1.0f / 30.0f);
        for (const Command& c : g.command_log) {
            went_home = went_home || (c.kind == CommandKind::SetBehavior &&
                                      c.param_a == static_cast<int32_t>(ActivityId::GoHome));
        }
    }
    CHECK(went_home);
}

TEST_CASE("hero_behavior: wasm hero: a hero with an apothecary in town decides Buy") {
    auto owned = testfix::make_wasm_world();
    BadlandsGame& g = *owned;
    REQUIRE(g.wasm_brains != nullptr);

    REQUIRE(place_building(g, BuildingKind::Apothecary, 0.0f, 20.0f) != UINT32_MAX);

    const uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, kCastleSpawnZ));
    const entt::entity e = g.slots[slot];
    // "Low inventory": a fresh hero starts empty (collect-only, capped at
    // kInventoryCap) -- exactly the state score_buy requires.
    CHECK(g.registry.get<HeroSimulationState>(e).inventory == 0);

    bool decided_buy = false;
    for (int i = 0; i < 10 && !decided_buy; ++i) {
        tick_world(g, 1.0f / 30.0f);
        for (const Command& c : g.command_log) {
            decided_buy = decided_buy || (c.kind == CommandKind::SetBehavior &&
                                          c.param_a == static_cast<int32_t>(ActivityId::Buy));
        }
    }
    CHECK(decided_buy);
}
