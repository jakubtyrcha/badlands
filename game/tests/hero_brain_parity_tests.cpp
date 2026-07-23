// Task 5 (wasm-brain feature): the ported hero decision layer
// (scripts/brains/nim/hero.nim -- game/src/town_brain.cpp +
// game/src/behaviours/{blocks,selectors,deliberation}.cpp transcribed to
// Nim/wasm) proven against its C++ reference.
//
// Twin-brain parity is the centerpiece: two Sims, identically seeded, one
// driven by town_think (the C++ mock brain) and one by the REAL, shipping
// assets/brains/hero.wasm -- their command logs must agree command-for-
// command over a real run. The behavioural smokes below it exercise the
// wasm side alone, mirroring needs_tests.cpp/townfolk_tests.cpp's own
// fixture shapes (force the reserve, tick, assert the resulting decision).

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <set>
#include <vector>

using namespace badlands;

namespace {

// Reads a binary wasm fixture, repo-root-relative like every other asset
// path in this codebase (add_test sets WORKING_DIRECTORY to the repo root).
// Duplicated from wasm_brain_tests.cpp per this file's own
// fixture-independence convention.
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

// Castle (auto, make_world) + Free Company Quarters + Hunter's Camp (two
// distinct recruiting classes) + Tavern + Apothecary, with a roster recruited
// from each guild. Mirrors determinism_tests.cpp's seed_town / wasm_brain_
// tests.cpp's seed_heroes fixture shape; duplicated per this file's own
// fixture-independence convention. A Hunter's high explore appetite (85%,
// activity_catalog.cpp) plus every class's flat Buy/Roam floor gets variety
// (Explore/Buy/Roam/GoHome/Chat/VisitTavern/Hunt) well inside the run below.
//
// Coordinates: on the Plains south of the lake, around the (auto-placed)
// Castle at (kCastleSpawnX, kCastleSpawnZ) = (0, 54) -- NOT determinism_tests.
// cpp's seed_town coordinates ((+-14, +-8)), which sit in the LAKE biome
// (world origin) and are fine for that file's state-reproducibility-only
// assertions but would leave every hero here stuck at its spawn point
// (terrain-blocked, nowhere to walk), starving this test's whole point:
// actually exercising the decision layer.
void seed_town(Sim& sim) {
    struct Seed {
        int kind;
        float x, z;
        int recruits;
    };
    const Seed kSeed[] = {
        {static_cast<int>(BuildingKind::FreeCompanyQuarters), -20.0f, 40.0f, 3},
        {static_cast<int>(BuildingKind::HuntersCamp), 20.0f, 40.0f, 3},
        {static_cast<int>(BuildingKind::Tavern), -20.0f, 68.0f, 0},
        {static_cast<int>(BuildingKind::Apothecary), 20.0f, 68.0f, 0},
    };
    for (const Seed& s : kSeed) {
        Action place{ActionKind::PlaceBuilding, 0, s.x, s.z, s.kind, 0};
        const int64_t id = sim.Dispatch(place);
        REQUIRE(id >= 0);
        for (int i = 0; i < s.recruits; ++i) {
            Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(id), 0.0f, 0.0f, 0, 0};
            REQUIRE(sim.Dispatch(recruit) >= 0);
        }
    }
}

}  // namespace

// --- twin-brain parity -------------------------------------------------------

TEST_CASE("twin brain: wasm hero decisions match the C++ reference command-for-command") {
    std::vector<uint8_t> bytes = read_hero_wasm();

    Sim mock(nullptr);  // BrainDesc{}: no noiser, no wasm -> heroes run town_think
    Sim wasm(wasm_desc(bytes));

    seed_town(mock);
    seed_town(wasm);

    constexpr int kTicks = 1200;  // well past the >= 300 the spec asks for
    for (int i = 0; i < kTicks; ++i) {
        mock.Tick(1.0f / 30.0f);
        wasm.Tick(1.0f / 30.0f);
    }

    CHECK(wasm.GetStats().noiser_bugs == 0);  // no wasm errors anywhere in the run

    const std::vector<CommandRecord> a = mock.CommandLog();
    const std::vector<CommandRecord> b = wasm.CommandLog();
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());

    // Sanity guard: the run must have actually exercised variety, or this
    // proves nothing (a vacuous all-Idle parity pass must fail this).
    std::set<int32_t> non_idle_activities;

    for (size_t i = 0; i < a.size(); ++i) {
        INFO("command " << i << " kind=" << static_cast<int32_t>(a[i].kind));
        // kind/actor/target/params/timing: EXACT.
        CHECK(a[i].kind == b[i].kind);
        CHECK(a[i].actor == b[i].actor);
        CHECK(a[i].target_id == b[i].target_id);
        CHECK(a[i].param_a == b[i].param_a);
        CHECK(a[i].param_b == b[i].param_b);
        CHECK(a[i].at_millis == b[i].at_millis);
        // point_x/point_z: within 1e-4 -- tolerance solely for potential
        // fp-contract differences between native arm64 C++ and wasm (see
        // task-5-brief.md); integer-derived floats should in practice be exact.
        CHECK(std::abs(a[i].point_x - b[i].point_x) <= 1e-4f);
        CHECK(std::abs(a[i].point_z - b[i].point_z) <= 1e-4f);

        if (a[i].kind == CommandKindId::SetBehavior &&
            a[i].param_a != static_cast<int32_t>(ActivityId::Idle)) {
            non_idle_activities.insert(a[i].param_a);
        }
    }

    CHECK(non_idle_activities.size() >= 3);
}

// --- behavioural smokes (wasm side only) -------------------------------------

TEST_CASE("wasm hero: an exhausted, homed hero decides GoHome") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    Action place{ActionKind::PlaceBuilding, 0, -14.0f, -8.0f,
                static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    const int64_t guild = dispatch_into(*g, place);
    REQUIRE(guild >= 0);
    Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(guild), 0.0f, 0.0f, 0, 0};
    const int64_t hero_id = dispatch_into(*g, recruit);
    REQUIRE(hero_id >= 0);
    entt::entity e = g->slots[static_cast<size_t>(hero_id)];

    // Force exhaustion directly (needs_tests.cpp's own pattern) rather than
    // ticking out a real drain -- what is under test is "does GoHome fire
    // once fatigue crosses the bar", not how long that naturally takes.
    g->registry.get<HeroSimulationState>(e).fatigue = 0.05f;

    bool went_home = false;
    for (int i = 0; i < 30 && !went_home; ++i) {
        tick_world(*g, 1.0f / 30.0f);
        for (const Command& c : g->command_log) {
            went_home = went_home || (c.kind == CommandKind::SetBehavior &&
                                      c.param_a == static_cast<int32_t>(ActivityId::GoHome));
        }
    }
    CHECK(went_home);
    CHECK(stats_of(*g).noiser_bugs == 0);
}

TEST_CASE("wasm hero: a hero with an apothecary in town decides Buy") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    Action place{ActionKind::PlaceBuilding, 0, 0.0f, 20.0f,
                static_cast<int32_t>(BuildingKind::Apothecary), 0};
    REQUIRE(dispatch_into(*g, place) >= 0);

    uint32_t slot = spawn_into(*g, MercenaryDesc(0.0f, kCastleSpawnZ));
    entt::entity e = g->slots[slot];
    // "Low inventory": a fresh hero starts empty (collect-only, capped at
    // kInventoryCap) -- exactly the state score_buy requires.
    CHECK(g->registry.get<HeroSimulationState>(e).inventory == 0);

    bool decided_buy = false;
    for (int i = 0; i < 10 && !decided_buy; ++i) {
        tick_world(*g, 1.0f / 30.0f);
        for (const Command& c : g->command_log) {
            decided_buy = decided_buy || (c.kind == CommandKind::SetBehavior &&
                                          c.param_a == static_cast<int32_t>(ActivityId::Buy));
        }
    }
    CHECK(decided_buy);
    CHECK(stats_of(*g).noiser_bugs == 0);
}
