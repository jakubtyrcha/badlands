// Pure-simulation tests: mock brains only, no noiser involvement. These must
// stay green regardless of the scripting language's health — they are the
// spec of the Stage-2 combat mechanics.

#include "badlands_game.h"
#include "duel_common.h"

#include <catch_amalgamated.hpp>

#include <cmath>

using namespace testfix;

namespace {

GameCharacterDesc dummy(float x, float z, int32_t team) {
    GameCharacterDesc desc{};
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

TEST_CASE("game_state echoes the spawn descriptor") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc desc = mercenary(-8.0f, -12.0f);
    uint32_t id = game_spawn(game, &desc);

    auto rows = snapshot(game);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == id);
    CHECK(rows[0].team == 0);
    CHECK(rows[0].pos_x == desc.pos_x);
    CHECK(rows[0].pos_z == desc.pos_z);
    CHECK(rows[0].hp == desc.hp);
    CHECK(rows[0].max_hp == desc.hp);
    CHECK(rows[0].size_y == desc.size_y);
    CHECK(rows[0].color_b == desc.color_b);

    game_destroy(game);
}

TEST_CASE("movement is clamped to move_speed * dt") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc a = dummy(0.0f, 0.0f, 0);
    GameCharacterDesc b = dummy(10.0f, 0.0f, 1);
    game_spawn(game, &a);
    game_spawn(game, &b);

    game_tick(game, kTickDt);

    auto rows = snapshot(game);
    REQUIRE(rows.size() == 2);
    float step = a.move_speed * kTickDt;
    CHECK_THAT(rows[0].pos_x, Catch::Matchers::WithinAbs(step, 1e-4f));
    CHECK(rows[0].pos_z == 0.0f);
    CHECK_THAT(rows[1].pos_x, Catch::Matchers::WithinAbs(10.0f - step, 1e-4f));

    game_destroy(game);
}

TEST_CASE("attacks respect the cooldown") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc attacker = dummy(0.0f, 0.0f, 0);
    attacker.attack_damage = 3.0f;
    attacker.attack_cooldown = 1.0f;
    GameCharacterDesc victim = dummy(0.5f, 0.0f, 1);  // in range from tick one
    victim.hp = 100.0f;
    victim.attack_damage = 0.0f;  // never hurts the attacker
    game_spawn(game, &attacker);
    game_spawn(game, &victim);

    // dt of 0.25 is binary-exact, so the cooldown arithmetic is deterministic.
    game_tick(game, 0.25f);
    CHECK(snapshot(game)[1].hp == 97.0f);  // first swing lands immediately

    // Cooldown (1s) blocks the swing while it drains: 0.75, 0.5, 0.25.
    for (int i = 0; i < 3; ++i) {
        game_tick(game, 0.25f);
    }
    CHECK(snapshot(game)[1].hp == 97.0f);

    game_tick(game, 0.25f);  // cooldown reaches zero -> swing lands
    CHECK(snapshot(game)[1].hp == 94.0f);

    game_destroy(game);
}

TEST_CASE("dead entities leave the state snapshot") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc killer = dummy(0.0f, 0.0f, 0);
    killer.attack_damage = 100.0f;
    GameCharacterDesc victim = dummy(0.5f, 0.0f, 1);
    victim.attack_damage = 0.0f;
    uint32_t killer_id = game_spawn(game, &killer);
    game_spawn(game, &victim);

    game_tick(game, kTickDt);

    auto rows = snapshot(game);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == killer_id);

    game_destroy(game);
}

TEST_CASE("game_state reports the total beyond the caller's cap") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc desc = dummy(0.0f, 0.0f, 0);  // one team: nobody fights
    for (int i = 0; i < 300; ++i) {
        game_spawn(game, &desc);
    }

    GameCharacterState rows[8];
    CHECK(game_state(game, rows, 8) == 300);      // total, not written count
    CHECK(snapshot(game).size() == 300);          // the helper must not cap

    game_destroy(game);
}

TEST_CASE("Stage-2 duel resolves with mock brains") {
    BadlandsGame* game = game_create(nullptr);
    GameCharacterDesc merc = mercenary(-8.0f, -12.0f);
    GameCharacterDesc gob = goblin(8.0f, -12.0f);
    uint32_t merc_id = game_spawn(game, &merc);
    game_spawn(game, &gob);

    GameCharacterState survivor = run_duel(game);

    CHECK(survivor.id == merc_id);
    CHECK(survivor.team == 0);
    CHECK(survivor.hp < survivor.max_hp);  // the goblin got its licks in

    GameStats stats;
    game_stats(game, &stats);
    CHECK(stats.ticks > 30);  // spawned 16 units apart: they must walk first
    CHECK(stats.script_intents == 0);
    CHECK(stats.noiser_bugs == 0);

    game_destroy(game);
}
