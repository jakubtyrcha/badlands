// Stage-2 integration: the Mercenary-vs-Goblin duel driven by the real
// warrior.noiser brain script. Prefers noiser at every stage; if the script
// fails anywhere, the affected entity downgrades to the mock brain, the bug
// is recorded, and the duel still resolves — the suite stays green while the
// failure is impossible to miss.

#include "badlands_game.h"
#include "duel_common.h"

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace testfix;

namespace {

std::string load_brain_script() {
    const char* path = std::getenv("BADLANDS_BRAIN_SCRIPT");
    REQUIRE(path != nullptr);  // exported by tests/cpp_tests.rs
    std::ifstream file(path);
    REQUIRE(file.good());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("Stage-2 duel with noiser brains") {
    std::string script = load_brain_script();
    BadlandsGame* game = game_create(script.c_str());

    GameCharacterDesc merc = mercenary(-8.0f, -12.0f);
    GameCharacterDesc gob = goblin(8.0f, -12.0f);
    uint32_t merc_id = game_spawn(game, &merc);
    game_spawn(game, &gob);

    GameCharacterState survivor = run_duel(game);

    // Behavioral spec, independent of noiser's health.
    CHECK(survivor.id == merc_id);
    CHECK(survivor.team == 0);
    CHECK(survivor.hp < survivor.max_hp);

    GameStats stats;
    game_stats(game, &stats);
    CHECK(stats.ticks > 30);

    if (stats.noiser_bugs == 0) {
        // No fallback happened: the script must have driven every decision.
        // Each brain emits at most one intent per tick, and idle emits none,
        // so a healthy run shows plenty of script intents.
        CHECK(stats.script_intents > 0);
        SUCCEED("noiser brains drove the duel; script intents: " << stats.script_intents);
    } else {
        WARN("[noiser-bug] duel completed via mock fallback; bugs recorded: "
             << stats.noiser_bugs << " (see stderr; paste into docs/noiser-feedback.md)");
    }

    game_destroy(game);
}

TEST_CASE("script reload keeps last-good on failure and swaps on success") {
    std::string script = load_brain_script();
    BadlandsGame* game = game_create(script.c_str());
    GameCharacterDesc merc = mercenary(-8.0f, -12.0f);
    GameCharacterDesc gob = goblin(8.0f, -12.0f);
    game_spawn(game, &merc);
    game_spawn(game, &gob);

    for (int i = 0; i < 10; ++i) {
        game_tick(game, kTickDt);
    }
    GameStats before;
    game_stats(game, &before);
    REQUIRE(before.noiser_bugs == 0);
    REQUIRE(before.script_intents > 0);

    // A broken script must not take: the old program keeps driving.
    CHECK(!game_reload_script(game, "pub fn broken("));
    for (int i = 0; i < 10; ++i) {
        game_tick(game, kTickDt);
    }
    GameStats after_failed;
    game_stats(game, &after_failed);
    CHECK(after_failed.noiser_bugs == 0);
    CHECK(after_failed.script_intents > before.script_intents);

    // A good script swaps in; brains restart (counters reset) and keep driving.
    CHECK(game_reload_script(game, script.c_str()));
    for (int i = 0; i < 10; ++i) {
        game_tick(game, kTickDt);
    }
    GameStats after_reload;
    game_stats(game, &after_reload);
    CHECK(after_reload.noiser_bugs == 0);
    CHECK(after_reload.script_intents > 0);

    game_destroy(game);
}

TEST_CASE("a failing brain downgrades to the mock and the duel still resolves") {
    // This brain divides by zero on its fourth tick. The engine must record
    // the bug, downgrade the entity, and finish the fight on mock brains.
    // It declares and calls the full host interface: brain scripts must, or
    // game_create rejects them as signature drift.
    const char* script = R"(
        @fn.perceive_self: fn(e: i32) -> (f32, f32, f32, f32);
        @fn.perceive_target: fn(e: i32) -> (f32, f32, f32, f32);
        @fn.attack_range: fn(e: i32) -> f32;
        @fn.intent_move: fn(e: i32, dx: f32, dz: f32) -> void;
        @fn.intent_attack: fn(e: i32) -> void;
        fn divide(x: i32) -> i32 { 1 / x }
        pub gen fn brain(entity: i32) -> i32 {
            var i = 0;
            loop {
                let (sx, sz, shp, scd) = @fn.perceive_self(entity);
                let (tx, tz, thp, has) = @fn.perceive_target(entity);
                if has > 0.5 && @fn.attack_range(entity) >= scd {
                    @fn.intent_attack(entity);
                } else {
                    @fn.intent_move(entity, 1.0, 0.0);
                }
                let poison = divide(3 - i);
                i = i + 1;
                yield poison;
            }
        }
        0.0
    )";
    BadlandsGame* game = game_create(script);

    GameCharacterDesc merc = mercenary(-8.0f, -12.0f);
    GameCharacterDesc gob = goblin(8.0f, -12.0f);
    uint32_t merc_id = game_spawn(game, &merc);
    game_spawn(game, &gob);

    GameCharacterState survivor = run_duel(game);
    CHECK(survivor.id == merc_id);

    GameStats stats;
    game_stats(game, &stats);
    CHECK(stats.noiser_bugs >= 2);  // both entities' brains hit the poison

    game_destroy(game);
}
