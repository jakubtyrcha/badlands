// The transient game-event stream (badlands::GameEvent / Sim::DrainEvents):
// notable things that HAPPENED -- damage, downing -- surfaced for the renderer's
// floating combat text and the HUD combat log. Driven by the MOCK brains (no
// noiser script needed): two hostile fighters placed in range trade blows every
// tick their cooldown allows, so the stream is fully deterministic here.

#include "badlands_sim.hpp"

#include <catch_amalgamated.hpp>

#include <vector>

using badlands::CharacterDesc;
using badlands::GameEvent;
using badlands::GameEventKind;
using badlands::Sim;

namespace {

constexpr float kTickDt = 1.0f / 30.0f;

// Merc (team 0, 30hp, 4dmg) faces Goblin (team 1, 18hp, 2dmg) at ~1u apart --
// inside both attack ranges, so both swing on the first eligible tick.
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

}  // namespace

TEST_CASE("a landed melee hit emits one attributed DamageDealt event") {
    Duel d;
    std::vector<GameEvent> evs;

    d.sim.Tick(kTickDt);
    d.sim.DrainEvents(evs);

    // Merc -> Goblin, for the merc's attack_damage (4).
    const GameEvent* mg =
        find(evs, GameEventKind::DamageDealt, d.merc, d.gob);
    REQUIRE(mg != nullptr);
    CHECK(mg->amount == Catch::Approx(4.0f));
    CHECK(mg->target_kind == badlands::kEventTargetCharacter);
    // Victim position was carried (goblin sits near (0,1)).
    CHECK(mg->z == Catch::Approx(1.0f).margin(0.5f));

    // Goblin -> Merc, for the goblin's attack_damage (2).
    const GameEvent* gm =
        find(evs, GameEventKind::DamageDealt, d.gob, d.merc);
    REQUIRE(gm != nullptr);
    CHECK(gm->amount == Catch::Approx(2.0f));
}

TEST_CASE("DrainEvents empties the buffer") {
    Duel d;
    std::vector<GameEvent> evs;

    d.sim.Tick(kTickDt);
    d.sim.DrainEvents(evs);
    REQUIRE(!evs.empty());

    // No tick between drains -> nothing new.
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
