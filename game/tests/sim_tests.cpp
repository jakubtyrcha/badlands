// Pure-simulation tests: mock brains only (BrainDesc{}), no wasm brain
// involved. The spec of the Stage-2 combat mechanics.

#include "badlands_sim.hpp"
#include "duel_common.h"

#include <catch_amalgamated.hpp>

#include <cmath>

using namespace testfix;

namespace {

badlands::CharacterDesc dummy(float x, float z, int32_t team) {
    badlands::CharacterDesc desc{};
    desc.pos_x = x;
    desc.pos_z = z;
    desc.team = team;
    desc.hp = 10.0f;
    desc.move_speed = 2.0f;
    desc.attack_range = 1.0f;
    desc.attack_damage = 1.0f;
    desc.attack_cooldown = 1.0f;
    desc.size_x = desc.size_y = desc.size_z = 1.0f;
    return desc;
}

}  // namespace

TEST_CASE("Characters(out) fills identically to Characters() and reuses the buffer") {
    badlands::Sim sim(badlands::BrainDesc{});
    sim.Spawn(dummy(3.0f, 0.0f, 0));
    sim.Spawn(dummy(-4.0f, 2.0f, 1));

    const std::vector<badlands::CharacterState> value = sim.Characters();
    std::vector<badlands::CharacterState> out;
    sim.Characters(out);

    REQUIRE(out.size() == 2u);
    REQUIRE(out.size() == value.size());
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].id == value[i].id);
        CHECK(out[i].pos_x == value[i].pos_x);
        CHECK(out[i].pos_z == value[i].pos_z);
        CHECK(out[i].team == value[i].team);
    }

    // The fill overload clears + refills (never appends) and keeps its capacity,
    // so the per-frame render path pays no fresh allocation.
    const size_t cap = out.capacity();
    sim.Characters(out);
    CHECK(out.size() == 2u);
    CHECK(out.capacity() >= cap);
}

TEST_CASE("Characters() echoes the spawn descriptor") {
    badlands::Sim sim(badlands::BrainDesc{});
    badlands::CharacterDesc desc = mercenary(-8.0f, kDuelGroundZ);
    uint32_t id = sim.Spawn(desc);

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == id);
    CHECK(rows[0].team == 0);
    CHECK(rows[0].pos_x == desc.pos_x);
    CHECK(rows[0].pos_z == desc.pos_z);
    CHECK(rows[0].hp == desc.hp);
    CHECK(rows[0].max_hp == desc.hp);
    CHECK(rows[0].size_y == desc.size_y);
    CHECK(rows[0].color_b == desc.color_b);
}

TEST_CASE("movement is clamped to move_speed * dt") {
    badlands::Sim sim(badlands::BrainDesc{});
    // Clear plains, away from BOTH the central lake AND the prebuilt castle at
    // kCastleSpawn (0, 54): the navmesh routes around the castle, so a unit told
    // to walk onto it would detour and deviate off the X axis. z = 30 keeps the
    // straight X corridor clear, and this case is only about the speed clamp.
    constexpr float kClearZ = 30.0f;
    // Single-gateway combat: a brainless Hero (dummy()'s default archetype)
    // issues no intentions/actions and never engages -- `a` needs a fighting
    // brain to have anywhere to walk on its own, so it is a Monster here
    // (its simple brain, monster_brain.cpp, restates Attack and the
    // engagement executor, apply_intention's Attack case, walks it toward
    // `b`). `b` stays brainless and holds position -- this case is only
    // about the speed clamp on the one unit that actually moves.
    badlands::CharacterDesc a = dummy(0.0f, kClearZ, 0);
    a.archetype = badlands::Archetype::Monster;
    badlands::CharacterDesc b = dummy(10.0f, kClearZ, 1);
    sim.Spawn(a);
    sim.Spawn(b);

    sim.Step();

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 2);
    float step = a.move_speed * kTickDt;
    CHECK_THAT(rows[0].pos_x, Catch::Matchers::WithinAbs(step, 1e-4f));
    CHECK(rows[0].pos_z == kClearZ);  // closes along X only
    CHECK(rows[1].pos_x == 10.0f);    // b (brainless) holds position
}

TEST_CASE("attacks respect the cooldown") {
    badlands::Sim sim(badlands::BrainDesc{});
    // Single-gateway combat: only a brain-driven actor swings -- Monster
    // here (its simple brain, monster_brain.cpp, resolve_action-gates on
    // the SAME attack_usable cooldown check this test pins).
    badlands::CharacterDesc attacker = dummy(0.0f, 0.0f, 0);
    attacker.archetype = badlands::Archetype::Monster;
    attacker.attack_damage = 3.0f;
    attacker.attack_cooldown = 1.0f;
    badlands::CharacterDesc victim = dummy(0.5f, 0.0f, 1);  // in range from tick one
    victim.hp = 100.0f;
    victim.attack_damage = 0.0f;  // never hurts the attacker
    sim.Spawn(attacker);
    sim.Spawn(victim);

    // A step is a FIXED span of sim time now (CLAUDE.md's time convention), so
    // this counts real steps instead of fabricating a convenient 0.25 s dt --
    // which is the whole point of the parameter being gone.
    sim.Step();
    CHECK(sim.Characters()[1].hp == 97.0f);  // first swing lands immediately

    // The 1 s cooldown blocks the swing for exactly one second of sim time,
    // which is kStepHz steps -- expressed as the rate rather than a count, so
    // the test still means "one second" if the step rate ever moves.
    const int steps_per_second = static_cast<int>(badlands::kStepsPerSecond);
    for (int i = 0; i < steps_per_second - 1; ++i) {
        sim.Step();
    }
    CHECK(sim.Characters()[1].hp == 97.0f);

    sim.Step();  // cooldown reaches zero -> swing lands
    CHECK(sim.Characters()[1].hp == 94.0f);
}

TEST_CASE("dead entities leave the state snapshot") {
    badlands::Sim sim(badlands::BrainDesc{});
    // Single-gateway combat: the killer needs a fighting brain to swing at
    // all -- Monster here, same reasoning as "attacks respect the cooldown"
    // above.
    badlands::CharacterDesc killer = dummy(0.0f, 0.0f, 0);
    killer.archetype = badlands::Archetype::Monster;
    killer.attack_damage = 100.0f;
    badlands::CharacterDesc victim = dummy(0.5f, 0.0f, 1);
    victim.attack_damage = 0.0f;
    uint32_t killer_id = sim.Spawn(killer);
    sim.Spawn(victim);

    sim.Step();

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == killer_id);
}

TEST_CASE("Characters() reports every spawned entity, no cap") {
    badlands::Sim sim(badlands::BrainDesc{});
    badlands::CharacterDesc desc = dummy(0.0f, 0.0f, 0);  // one team: nobody fights
    for (int i = 0; i < 300; ++i) {
        sim.Spawn(desc);
    }

    CHECK(sim.Characters().size() == 300);  // the accessor must not cap
}

TEST_CASE("Stage-2 duel resolves with mock brains: the goblin (a brain) wins, "
          "the brainless mercenary never swings") {
    // Single-gateway combat (docs/superpowers/specs/2026-07-25-contract-v3-
    // alignment-design.md): attacking is an ACTION, so a brainless mercenary
    // (BrainDesc{} loads no wasm brain, and Hero has no C++ decision layer
    // left) issues no intentions/actions at all -- it only rolls passive
    // defense (resolve_attack's defender gates). The goblin's simple brain
    // (Monster, monster_brain.cpp) is the one side that actually fights, so
    // it is the one that must win, and at FULL health: the merc never lands
    // a hit back.
    badlands::Sim sim(badlands::BrainDesc{});
    badlands::CharacterDesc merc = mercenary(-8.0f, kDuelGroundZ);
    badlands::CharacterDesc gob = goblin(8.0f, kDuelGroundZ);
    sim.Spawn(merc);
    uint32_t gob_id = sim.Spawn(gob);

    badlands::CharacterState survivor = run_duel(sim);

    CHECK(survivor.id == gob_id);
    CHECK(survivor.team == 1);
    CHECK(survivor.hp == survivor.max_hp);  // the brainless merc never landed a swing

    badlands::SimStats stats = sim.GetStats();
    CHECK(stats.ticks > 30);  // spawned 16 units apart: they must walk first
}
