// The transient game-event stream (badlands::GameEvent / Sim::DrainEvents):
// notable things that HAPPENED -- damage, downing -- surfaced for the renderer's
// floating combat text and the HUD combat log. Driven by the MOCK brains (no
// wasm brain needed): a goblin (Monster, its simple brain, monster_brain.cpp)
// faces a brainless mercenary placed in range.
//
// Single-gateway combat (docs/superpowers/specs/2026-07-25-contract-v3-
// alignment-design.md): attacking is an ACTION, so the brainless merc issues
// none and never swings -- only the goblin, which has a (simple) brain,
// deals damage. The fight is deliberately ONE-DIRECTIONAL now; see each
// case's own comment for what that means for its assertions.
//
// Combat now ROLLS to hit/dodge/crit (seeded), so a given swing may miss and the
// exact damage varies -- these tests therefore assert the event stream's SHAPE
// (attribution, kind, drain semantics) over the fight, not a single tick's numbers.

#include "badlands_sim.hpp"

#include <catch_amalgamated.hpp>

#include <vector>

using badlands::BrainDesc;
using badlands::CharacterDesc;
using badlands::GameEvent;
using badlands::GameEventKind;
using badlands::Sim;

namespace {

constexpr float kTickDt = 1.0f / 30.0f;

// Merc (team 0) faces Goblin (team 1) at ~1u apart -- inside both attack ranges,
// so they trade blows as their cooldowns and to-hit rolls allow.
struct Duel {
    Sim sim{BrainDesc{}};  // mock brains
    uint32_t merc, gob;
    Duel() {
        CharacterDesc m = badlands::MercenaryDesc(0.0f, 0.0f);
        CharacterDesc g = badlands::GoblinDesc(0.0f, 1.0f);
        merc = sim.Spawn(m);
        gob = sim.Spawn(g);
    }
};

const GameEvent* find(const std::vector<GameEvent>& evs, GameEventKind kind,
                      uint32_t actor, uint32_t target) {
    for (const GameEvent& e : evs) {
        if (e.kind == kind && e.actor_id == actor && e.target_id == target) {
            return &e;
        }
    }
    return nullptr;
}

// Tick until `find` locates the wanted event (accumulating drained events), or
// give up. Returns a copy of the event, or a disengaged optional.
template <class Pred>
const GameEvent* tick_until(Duel& d, std::vector<GameEvent>& all, Pred found, int max_ticks) {
    std::vector<GameEvent> evs;
    for (int i = 0; i < max_ticks; ++i) {
        d.sim.Tick(kTickDt);
        d.sim.DrainEvents(evs);
        for (const GameEvent& e : evs) {
            all.push_back(e);
        }
        if (const GameEvent* hit = found(all)) {
            return hit;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a landed melee hit emits one attributed DamageDealt event") {
    Duel d;
    std::vector<GameEvent> all;

    // The goblin (the one side with a brain) lands a hit within a few ticks;
    // the event is attributed to it, carries positive damage, targets the
    // merc as a character, at ~(0,0).
    const GameEvent* mg = tick_until(
        d, all,
        [&](const std::vector<GameEvent>& evs) {
            return find(evs, GameEventKind::DamageDealt, d.gob, d.merc);
        },
        60);
    REQUIRE(mg != nullptr);
    CHECK(mg->amount > 0.0f);
    CHECK(mg->target_kind == badlands::kEventTargetCharacter);
    CHECK(mg->z == Catch::Approx(0.0f).margin(0.6f));
}

TEST_CASE("DrainEvents empties the buffer") {
    Duel d;
    std::vector<GameEvent> all;
    // Tick until at least one event has been produced (a hit lands quickly).
    const GameEvent* any = tick_until(
        d, all, [](const std::vector<GameEvent>& evs) { return evs.empty() ? nullptr : &evs.back(); },
        60);
    REQUIRE(any != nullptr);

    // A drain with no tick in between yields nothing new.
    std::vector<GameEvent> evs;
    d.sim.DrainEvents(evs);
    CHECK(evs.empty());
}

TEST_CASE("a lethal blow emits DamageDealt plus HeroDowned for the loser") {
    Duel d;
    std::vector<GameEvent> all;
    std::vector<GameEvent> evs;

    // Single-gateway combat: the brainless merc never swings, so this is a
    // one-directional attrition, not a duel either side could win -- the
    // merc is the loser by construction. Downed within 3000 ticks (100s) is
    // generous: the goblin's ~0.8s cooldown and the merc's real
    // accuracy/evasion/armour easily clear 30 hp well inside that window.
    bool merc_downed = false;
    bool goblin_downed = false;
    for (int i = 0; i < 3000; ++i) {
        d.sim.Tick(kTickDt);
        d.sim.DrainEvents(evs);
        for (const GameEvent& e : evs) {
            if (e.kind == GameEventKind::HeroDowned) {
                if (e.target_id == d.merc) merc_downed = true;
                if (e.target_id == d.gob) goblin_downed = true;
            }
            all.push_back(e);
        }
        if (merc_downed) break;
    }

    // The goblin wins: the merc is downed, the goblin never even takes a hit.
    CHECK(merc_downed);
    CHECK_FALSE(goblin_downed);

    // The downing is attributed to the goblin and co-occurs with a DamageDealt.
    const GameEvent* down =
        find(all, GameEventKind::HeroDowned, d.gob, d.merc);
    REQUIRE(down != nullptr);
    CHECK(find(all, GameEventKind::DamageDealt, d.gob, d.merc) != nullptr);

    // The asymmetry, explicit: the brainless merc never once appears as the
    // ATTACKER of a DamageDealt event, over the whole fight.
    bool merc_ever_attacked = false;
    for (const GameEvent& e : all) {
        merc_ever_attacked =
            merc_ever_attacked || (e.kind == GameEventKind::DamageDealt && e.actor_id == d.merc);
    }
    CHECK_FALSE(merc_ever_attacked);
}
