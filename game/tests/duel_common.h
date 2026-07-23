// Shared fixtures for the badlands game tests: the Stage-2 duelist descs and
// the run-to-resolution helper.

#pragma once

#include "badlands_sim.hpp"

#include <catch_amalgamated.hpp>

#include <vector>

namespace testfix {

constexpr float kTickDt = 1.0f / 30.0f;
constexpr int kMaxTicks = 3000;  // 100 sim-seconds; the duel resolves in ~10

// The Stage-2 duelists — canonical stats live in the engine
// (badlands::MercenaryDesc/GoblinDesc), shared with the Rust app.
inline badlands::CharacterDesc mercenary(float x, float z) {
    return badlands::MercenaryDesc(x, z);
}

inline badlands::CharacterDesc goblin(float x, float z) {
    return badlands::GoblinDesc(x, z);
}

inline std::vector<badlands::CharacterState> snapshot(const badlands::Sim& sim) {
    return sim.Characters();
}

// Ticks until exactly one entity is left standing; fails the test on timeout
// or mutual destruction. Returns the survivor row.
inline badlands::CharacterState run_duel(badlands::Sim& sim) {
    for (int i = 0; i < kMaxTicks; ++i) {
        sim.Tick(kTickDt);
        auto rows = snapshot(sim);
        REQUIRE(!rows.empty());  // mutual destruction would be a combat bug
        if (rows.size() == 1) {
            return rows[0];
        }
    }
    FAIL("duel did not resolve within " << kMaxTicks << " ticks");
    return {};
}

}  // namespace testfix
