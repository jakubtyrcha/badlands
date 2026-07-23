// The transient game-event stream (badlands::GameEvent / Sim::DrainEvents):
// notable things that HAPPENED -- damage, downing -- surfaced for the renderer's
// floating combat text and the HUD combat log. Driven by the MOCK brains (no
// noiser script needed): two hostile fighters placed in range trade blows.
//
// Combat now ROLLS to hit/dodge/crit (seeded), so a given swing may miss and the
// exact damage varies -- these tests therefore assert the event stream's SHAPE
// (attribution, kind, drain semantics) over the fight, not a single tick's numbers.

#include "badlands_sim.hpp"

#include <catch_amalgamated.hpp>

#include <vector>

using badlands::CharacterDesc;
using badlands::GameEvent;
using badlands::GameEventKind;
using badlands::Sim;

namespace {

constexpr float kTickDt = 1.0f / 30.0f;

// Merc (team 0) faces Goblin (team 1) at ~1u apart -- inside both attack ranges,
// so they trade blows as their cooldowns and to-hit rolls allow.
struct Duel {
    Sim sim{nullptr};  // mock brains
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

    // The merc lands a hit within a few ticks; the event is attributed to it,
    // carries positive damage, targets the goblin as a character, at ~(0,1).
    const GameEvent* mg = tick_until(
        d, all,
        [&](const std::vector<GameEvent>& evs) {
            return find(evs, GameEventKind::DamageDealt, d.merc, d.gob);
        },
        60);
    REQUIRE(mg != nullptr);
    CHECK(mg->amount > 0.0f);
    CHECK(mg->target_kind == badlands::kEventTargetCharacter);
    CHECK(mg->z == Catch::Approx(1.0f).margin(0.6f));
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

    bool goblin_downed = false;
    bool merc_downed = false;
    for (int i = 0; i < 3000; ++i) {
        d.sim.Tick(kTickDt);
        d.sim.DrainEvents(evs);
        for (const GameEvent& e : evs) {
            if (e.kind == GameEventKind::HeroDowned) {
                if (e.target_id == d.gob) goblin_downed = true;
                if (e.target_id == d.merc) merc_downed = true;
            }
            all.push_back(e);
        }
        if (goblin_downed) break;
    }

    // The merc wins: the goblin is downed, the merc is not.
    CHECK(goblin_downed);
    CHECK_FALSE(merc_downed);

    // The downing is attributed to the merc and co-occurs with a DamageDealt.
    const GameEvent* down =
        find(all, GameEventKind::HeroDowned, d.merc, d.gob);
    REQUIRE(down != nullptr);
    CHECK(find(all, GameEventKind::DamageDealt, d.merc, d.gob) != nullptr);
}
