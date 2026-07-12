// Shared fixtures for the badlands game tests: the Stage-2 duelist descs and
// the run-to-resolution helper.

#pragma once

#include "badlands_game.h"

#include <catch_amalgamated.hpp>

#include <vector>

namespace testfix {

constexpr float kTickDt = 1.0f / 30.0f;
constexpr int kMaxTicks = 3000;  // 100 sim-seconds; the duel resolves in ~10

// The Stage-2 duelists — canonical stats live in the engine
// (game_desc_mercenary/game_desc_goblin), shared with the Rust app.
inline GameCharacterDesc mercenary(float x, float z) {
    return game_desc_mercenary(x, z);
}

inline GameCharacterDesc goblin(float x, float z) {
    return game_desc_goblin(x, z);
}

inline std::vector<GameCharacterState> snapshot(const BadlandsGame* game) {
    // game_state returns the total living count; one retry always suffices.
    std::vector<GameCharacterState> rows(64);
    uint32_t total = game_state(game, rows.data(), static_cast<uint32_t>(rows.size()));
    if (total > rows.size()) {
        rows.resize(total);
        total = game_state(game, rows.data(), static_cast<uint32_t>(rows.size()));
    }
    rows.resize(total);
    return rows;
}

// Ticks until exactly one entity is left standing; fails the test on timeout
// or mutual destruction. Returns the survivor row.
inline GameCharacterState run_duel(BadlandsGame* game) {
    for (int i = 0; i < kMaxTicks; ++i) {
        game_tick(game, kTickDt);
        auto rows = snapshot(game);
        REQUIRE(!rows.empty());  // mutual destruction would be a combat bug
        if (rows.size() == 1) {
            return rows[0];
        }
    }
    FAIL("duel did not resolve within " << kMaxTicks << " ticks");
    return {};
}

}  // namespace testfix
