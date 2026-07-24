// Task 9 (medium-review fixes): sanitize_factors, the single validation
// boundary set_factors_of (sim.cpp) applies to every SimFactors write --
// live SetFactors calls and, critically, the factors.json load path
// (LoadSimFactors is itself deliberately unvalidated -- see its own
// comments, src/game/factors_manifest.cpp). sanitize_factors is a file-local
// function with no header of its own, so every case here goes through the
// public surface (Sim::SetFactors/Factors) exactly like a real caller would.
//
// Each TEST_CASE below is one of the three red/green repros from the task
// brief: (a) an inverted think-pause pair, unit-level; (b) the same
// inversion driving a REAL wasm-brained hero through an actual Think pause
// (this one aborted the whole test binary before the fix -- see the task
// report for the captured abort output); (c) a negative memory TTL, which
// today empties EntityMemory every tick regardless of what was just seen.

#include "badlands_sim.hpp"
#include "components.h"     // HeroSimulationState -- private header, game/src
#include "entity_memory.h"  // EntityMemory (char_count/visible_now) -- private header, game/src

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <fstream>
#include <vector>

using namespace badlands;

namespace {

// Reads a binary wasm fixture, repo-root-relative like every other asset
// path in this codebase (add_test sets WORKING_DIRECTORY to the repo root).
// Duplicated from wasm_brain_tests.cpp / hero_brain_parity_tests.cpp per
// this file's own fixture-independence convention.
std::vector<uint8_t> read_wasm_file(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.data()), size));
    return bytes;
}

// The shipping brain artifact (LFS binary; scripts/brains/nim/hero.nim).
std::vector<uint8_t> read_hero_wasm() { return read_wasm_file("assets/brains/hero.wasm"); }

BrainDesc wasm_desc(const std::vector<uint8_t>& bytes) {
    return BrainDesc{.wasm_bytes = bytes.data(), .wasm_len = bytes.size()};
}

}  // namespace

// --- (a) unit: think_min/think_max pairing ----------------------------------

TEST_CASE("sanitize_factors: an inverted think-pause pair comes back min <= max") {
    Sim sim{BrainDesc{}};
    SimFactors f = sim.Factors();
    f.hero.think_min_millis = 5000;
    f.hero.think_max_millis = 1000;
    sim.SetFactors(f);

    const SimFactors& out = sim.Factors();
    CHECK(out.hero.think_min_millis <= out.hero.think_max_millis);
    // Specifically: max is left alone (already >= 0), min is pulled down into
    // range -- not the other way around (see sanitize_factors' doc comment).
    CHECK(out.hero.think_max_millis == 1000);
    CHECK(out.hero.think_min_millis == 1000);
}

// --- (b) the crash repro: a real wasm-brained Think pause -------------------

TEST_CASE("sanitize_factors: an inverted think-pause pair no longer aborts a wasm Think") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    Sim sim{wasm_desc(bytes)};

    SimFactors f = sim.Factors();
    f.hero.think_min_millis = 5000;
    f.hero.think_max_millis = 1000;
    sim.SetFactors(f);

    Action place{ActionKind::PlaceBuilding, 0, -14.0f, -8.0f,
                static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    const int64_t guild = sim.Dispatch(place);
    REQUIRE(guild >= 0);
    Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(guild), 0.0f, 0.0f, 0, 0};
    const int64_t hero_id = sim.Dispatch(recruit);
    REQUIRE(hero_id >= 0);

    // Tick 1: the hero's FIRST-EVER decision (current_activity starts at -1,
    // "none yet") never deliberates, whatever it picks (behaviours/
    // deliberation.cpp's is_discretionary(-1) == false) -- it lands on Roam,
    // the flat always-available fallback, with nothing else applicable yet.
    sim.Tick(1.0f / 30.0f);

    // Force a GENUINE activity change: crash the hero's fatigue so GoHome
    // outscores Roam starting next tick. A second, discretionary, non-Idle
    // switch is exactly what deliberate() requires to draw a pause -- see the
    // task brief's "a genuine activity change must fire".
    entt::registry& reg = sim.registry();
    entt::entity hero_entity = entt::null;
    for (auto [e, hero] : reg.view<HeroSimulationState>().each()) {
        hero_entity = e;
        (void)hero;
    }
    const bool found_hero = (hero_entity != entt::null);
    REQUIRE(found_hero);
    reg.get<HeroSimulationState>(hero_entity).fatigue = 0.05f;

    // TODAY (pre-fix): decode_decision (wasm_brain.cpp) sees a
    // pause_duration_millis of 5000 (behaviours/rng.h's range_i64 returns
    // `lo` unconditionally when hi <= lo) against a think_max_millis of
    // 1000, rejects the wire, and tick_wasm_brain escalates that to
    // brain_fatal() -- spdlog::critical + std::abort(). See the task report
    // for the captured output of that abort.
    bool saw_think = false;
    for (int i = 0; i < 30 && !saw_think; ++i) {
        sim.Tick(1.0f / 30.0f);
        for (const CommandRecord& c : sim.CommandLog()) {
            saw_think = saw_think || (c.kind == CommandKindId::SetBehavior &&
                                      c.param_a == static_cast<int32_t>(ActivityId::Think));
        }
    }

    CHECK(saw_think);
    CHECK(sim.GetStats().noiser_bugs == 0);
}

// --- (c) TTL: a negative memory_ttl_millis must not evict a same-tick sighting

TEST_CASE("sanitize_factors: a negative memory TTL still lets two mercenaries remember "
         "each other") {
    Sim sim{BrainDesc{}};
    SimFactors f = sim.Factors();
    f.hero.memory_ttl_millis = -1;
    sim.SetFactors(f);

    // Both Hero archetype (EntityMemory opt-in, heroes.cpp's spawn_entity),
    // within the Mercenary's 14-unit vision radius (creature_catalog.cpp) of
    // each other -- v1 EntityMemory is radius-only, so facing doesn't matter
    // (see entity_memory.cpp's own comment on Vision::cone_half_cos).
    sim.SpawnCreature(CreatureId::Mercenary, /*team=*/0, 0.0f, kCastleSpawnZ);
    sim.SpawnCreature(CreatureId::Mercenary, /*team=*/0, 5.0f, kCastleSpawnZ);
    sim.Tick(1.0f / 30.0f);

    // TODAY (pre-fix): ttl == -1 makes update_entity_memory's TTL sweep
    // (`now - last_seen_millis > ttl`) true even for an entry seen THIS
    // tick (age 0 > -1), so EntityMemory empties every tick regardless of
    // who is actually in sight.
    entt::registry& reg = sim.registry();
    int observers_checked = 0;
    for (auto [e, mem] : reg.view<EntityMemory>().each()) {
        REQUIRE(mem.char_count > 0);
        CHECK(mem.chars[0].visible_now);
        ++observers_checked;
    }
    REQUIRE(observers_checked == 2);
}
