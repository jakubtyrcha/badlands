// Pure-simulation tests: mock brains only, no noiser involvement. These must
// stay green regardless of the scripting language's health — they are the
// spec of the Stage-2 combat mechanics.

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

TEST_CASE("SetPathfinder back-fills every alive building") {
    badlands::Sim sim(nullptr);
    // make_world prebuilds the colony Castle (kCastleSpawn), so >=1 alive building.
    REQUIRE(sim.Buildings().size() >= 1);

    struct Recorder {
        int add_obstacle_calls = 0;
    } recorder;

    badlands::Pathfinder pf{};
    pf.ctx = &recorder;
    pf.add_obstacle = [](void* ctx, uint32_t, const float*, int32_t) {
        static_cast<Recorder*>(ctx)->add_obstacle_calls += 1;
    };
    pf.remove_obstacle = [](void*, uint32_t) {};  // non-null no-op
    pf.find_path = [](void*, float, float, float, float, float, uint32_t, float*,
                      int32_t) -> int32_t { return 0; };  // non-null no-op

    sim.SetPathfinder(pf);

    // notify_obstacle_added fires exactly once per alive building, and
    // Buildings() applies the same alive-filter, so the counts must match.
    REQUIRE(recorder.add_obstacle_calls == static_cast<int>(sim.Buildings().size()));
}

TEST_CASE("Characters() echoes the spawn descriptor") {
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc desc = mercenary(-8.0f, -12.0f);
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
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc a = dummy(0.0f, 0.0f, 0);
    badlands::CharacterDesc b = dummy(10.0f, 0.0f, 1);
    sim.Spawn(a);
    sim.Spawn(b);

    sim.Tick(kTickDt);

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 2);
    float step = a.move_speed * kTickDt;
    CHECK_THAT(rows[0].pos_x, Catch::Matchers::WithinAbs(step, 1e-4f));
    CHECK(rows[0].pos_z == 0.0f);
    CHECK_THAT(rows[1].pos_x, Catch::Matchers::WithinAbs(10.0f - step, 1e-4f));
}

TEST_CASE("attacks respect the cooldown") {
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc attacker = dummy(0.0f, 0.0f, 0);
    attacker.attack_damage = 3.0f;
    attacker.attack_cooldown = 1.0f;
    badlands::CharacterDesc victim = dummy(0.5f, 0.0f, 1);  // in range from tick one
    victim.hp = 100.0f;
    victim.attack_damage = 0.0f;  // never hurts the attacker
    sim.Spawn(attacker);
    sim.Spawn(victim);

    // dt of 0.25 is binary-exact, so the cooldown arithmetic is deterministic.
    sim.Tick(0.25f);
    CHECK(sim.Characters()[1].hp == 97.0f);  // first swing lands immediately

    // Cooldown (1s) blocks the swing while it drains: 0.75, 0.5, 0.25.
    for (int i = 0; i < 3; ++i) {
        sim.Tick(0.25f);
    }
    CHECK(sim.Characters()[1].hp == 97.0f);

    sim.Tick(0.25f);  // cooldown reaches zero -> swing lands
    CHECK(sim.Characters()[1].hp == 94.0f);
}

TEST_CASE("dead entities leave the state snapshot") {
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc killer = dummy(0.0f, 0.0f, 0);
    killer.attack_damage = 100.0f;
    badlands::CharacterDesc victim = dummy(0.5f, 0.0f, 1);
    victim.attack_damage = 0.0f;
    uint32_t killer_id = sim.Spawn(killer);
    sim.Spawn(victim);

    sim.Tick(kTickDt);

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == killer_id);
}

TEST_CASE("Characters() reports every spawned entity, no cap") {
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc desc = dummy(0.0f, 0.0f, 0);  // one team: nobody fights
    for (int i = 0; i < 300; ++i) {
        sim.Spawn(desc);
    }

    CHECK(sim.Characters().size() == 300);  // the accessor must not cap
}

TEST_CASE("Stage-2 duel resolves with mock brains") {
    badlands::Sim sim(nullptr);
    badlands::CharacterDesc merc = mercenary(-8.0f, -12.0f);
    badlands::CharacterDesc gob = goblin(8.0f, -12.0f);
    uint32_t merc_id = sim.Spawn(merc);
    sim.Spawn(gob);

    badlands::CharacterState survivor = run_duel(sim);

    CHECK(survivor.id == merc_id);
    CHECK(survivor.team == 0);
    CHECK(survivor.hp < survivor.max_hp);  // the goblin got its licks in

    badlands::SimStats stats = sim.GetStats();
    CHECK(stats.ticks > 30);  // spawned 16 units apart: they must walk first
    CHECK(stats.script_intents == 0);
    CHECK(stats.noiser_bugs == 0);
}
