// Task 5 (wasm-brain feature): the ported hero decision layer
// (scripts/brains/nim/hero.nim -- game/src/town_brain.cpp +
// game/src/behaviours/{blocks,selectors,deliberation}.cpp transcribed to
// Nim/wasm) proven against its C++ reference.
//
// Twin-brain parity is the centerpiece: two BadlandsGame worlds, identically
// seeded -- same buildings, same recruits, same tuning factors, same extra
// critter/vision setup, see setup_variety_fixture -- one driven by
// town_think (the C++ mock brain) and one by the REAL, shipping
// assets/brains/hero.wasm; their command logs must agree command-for-command
// over a real run. The behavioural smokes below it exercise the wasm side
// alone, mirroring needs_tests.cpp/townfolk_tests.cpp's own fixture shapes
// (force the reserve, tick, assert the resulting decision).

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "sim_internal.hpp"
#include "vision.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

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

uint32_t place(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    const int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t recruit_at(BadlandsGame& g, uint32_t building_id) {
    Action a{ActionKind::RecruitHero, building_id, 0.0f, 0.0f, 0, 0};
    const int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

// Neutral prey (mirrors hunter_tests.cpp's own spawn_deer). move_speed is
// deliberately slower than a recruited hero's (heroes.cpp's hero_desc gives
// every class 2.5, regardless of class) so a Hunter's chase, once started,
// can actually close to attack_range (1.3) instead of an endless flee/give-
// chase stalemate.
uint32_t spawn_deer(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 2;
    d.hp = 10.0f;
    d.move_speed = 1.4f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.7f;
    return spawn_into(g, d);
}

// Everything both sims of the twin-brain parity run share, applied in the
// identical order with identical values to each -- the determinism contract
// (state = f(initial config, command log, N ticks)) means any knob that
// differs between the two invalidates the whole comparison, so this is the
// single place both worlds' setup goes through rather than two independent
// call sites that could drift apart.
//
// What each piece is FOR (closing review findings across two rounds on this
// fixture: it claimed variety -- Explore/Buy/Roam/GoHome/Chat/VisitTavern/
// Hunt -- it did not structurally deliver; Hunt was unreachable with no
// critter ever spawned, GoHome/Chat/Explore were merely "eventually
// reachable" at the shipped need-drain rates which the run's tick budget
// never actually reached, and Roam -- despite being a flat, always-available
// score -- never won a single tick because Buy (weighted higher) always beat
// it at spawn and the cranked need drains below always beat it by the time
// Buy's own walk to the apothecary let go). The base town (castle, auto + two
// guilds of distinct hero classes + tavern + apothecary, on the Plains south
// of the lake -- NOT determinism_tests.cpp's own seed_town coordinates,
// which sit in the lake biome and leave every hero stuck at spawn) gets
// Buy/Idle/Think for free from tick 1 (flat scores, no setup needed). The
// rest needs an explicit push:
//   - Roam: pre-filling one merc's inventory so Buy is inapplicable to it
//     from tick 1 (see the poke right after the merc recruit loop below).
//   - Hunt, and its Shoot follow-up (the new BL_CMD_SHOOT wire path): a deer
//     spawned within a recruited Hunter's sight (HeroFactors::
//     hunt_sight_radius = 22) and flee-trigger range (CritterFactors::
//     flee_radius = 8), slow enough that the chase closes within the run.
//   - GoHome, and the Chat/VisitTavern content-driven pair (Chat's follow-up
//     is the other new wire path, BL_CMD_CHAT): fatigue_drain_hours/
//     content_drain_hours cranked via SetFactors so both reserves cross
//     their urgency thresholds well inside this test's tick budget, instead
//     of over the ~24/~12 in-game hours (thousands of ticks) the shipped
//     defaults would take -- and cranked UNEVENLY on purpose: content much
//     faster than fatigue (1.5h vs 10h). Chat's weight (2.0) loses to
//     GoHome's (3.0) whenever both are urgent, so an equal crank starves
//     Chat of a window -- fatigue urgency was already significant by the
//     time content became low enough for Chat to even apply, and GoHome won
//     every tick thereafter (verified empirically: the first attempt at this
//     fixture, both cranked to comparable rates, produced zero Chat
//     decisions over 2000 ticks despite content bottoming out at 0). The 3
//     heroes recruited from one guild all spawn at that guild's single door
//     tile (distance 0 from each other), so once content has dropped enough
//     for two of them, they are already within chat_radius (2.0) -- no
//     separate positioning needed for the Chat command itself to fire, only
//     for the SetBehavior(Chat) decision.
//   - Explore: ConfigureVision over the whole map (determinism_tests.cpp's
//     own "a run with fog of war and explorers replays exactly" pattern), so
//     a frontier actually exists just past wherever a recruited Hunter (85%
//     per-lease-window explore appetite, activity_catalog.cpp) has walked.
void setup_variety_fixture(BadlandsGame& g) {
    // Wide-open grid (the whole map): configuring it reveals nothing by
    // itself (only walking discovers texels) -- it gives
    // pick_exploration_target's 90-unit search radius room to find a
    // frontier past wherever heroes have actually been.
    configure_vision(g.vision, -128.0f, -128.0f, 256.0f, 256.0f, 1.0f);

    // A full reserve now drains in a few hundred ticks instead of the
    // shipped ~24/~12 in-game-hour defaults (thousands of ticks), so
    // GoHome/Chat/VisitTavern's thresholds are crossed WITHIN this run
    // rather than merely being reachable in principle.
    SimFactors f = g.factors;
    f.hero.fatigue_drain_hours = 10.0f;
    f.hero.content_drain_hours = 1.5f;
    set_factors_of(g, f);

    const uint32_t fcq = place(g, BuildingKind::FreeCompanyQuarters, -20.0f, 40.0f);
    REQUIRE(fcq != UINT32_MAX);
    uint32_t last_merc = UINT32_MAX;
    for (int i = 0; i < 3; ++i) {
        last_merc = recruit_at(g, fcq);
        REQUIRE(last_merc != UINT32_MAX);
    }
    // Roam (base_hero_weights, activity_catalog.cpp) is a flat 1.0 fallback
    // that only wins a tick where nothing costlier applies -- and structurally
    // it never gets one here: Buy (1.5, flat) always outscores it at spawn
    // (every recruit starts with inventory 0 < kInventoryCap), and by the time
    // that first Buy trip would free it up again, the cranked need drains
    // above have already pushed GoHome/VisitTavern/Chat's weighted urgency
    // past it too. Pre-filling one merc's pack takes Buy out of contention for
    // it from tick 1 while fatigue/content are still full (zero urgency), so
    // Roam -- the only other flat activity a Mercenary reliably has (Explore's
    // appetite is a mere 5%, activity_catalog.cpp) -- wins outright. Not a
    // Command (like configure_vision/set_factors_of above): part of the
    // initial config both worlds' setup_variety_fixture call applies
    // identically, not the replayed log.
    g.registry.get<HeroSimulationState>(g.slots[last_merc]).inventory = kInventoryCap;

    const uint32_t camp = place(g, BuildingKind::HuntersCamp, 20.0f, 40.0f);
    REQUIRE(camp != UINT32_MAX);
    uint32_t first_hunter = UINT32_MAX;
    for (int i = 0; i < 3; ++i) {
        const uint32_t hid = recruit_at(g, camp);
        REQUIRE(hid != UINT32_MAX);
        if (first_hunter == UINT32_MAX) {
            first_hunter = hid;
        }
    }

    REQUIRE(place(g, BuildingKind::Tavern, -20.0f, 68.0f) != UINT32_MAX);
    REQUIRE(place(g, BuildingKind::Apothecary, 20.0f, 68.0f) != UINT32_MAX);

    // A deer within the first Hunter's sight AND flee-trigger range, so both
    // the Hunt activity and its Shoot follow-up fire during the run (not
    // just "a critter happens to exist somewhere on the map").
    const glm::vec2 hunter_pos = g.registry.get<Position>(g.slots[first_hunter]).pos;
    spawn_deer(g, hunter_pos + glm::vec2{6.0f, 0.0f});
}

}  // namespace

// --- twin-brain parity -------------------------------------------------------

TEST_CASE("twin brain: wasm hero decisions match the C++ reference command-for-command") {
    std::vector<uint8_t> bytes = read_hero_wasm();

    auto mock_owned = make_world(BrainDesc{});  // BrainDesc{}: heroes run town_think
    auto wasm_owned = make_world(wasm_desc(bytes));
    BadlandsGame* mock = mock_owned.get();
    BadlandsGame* wasm = wasm_owned.get();
    REQUIRE(wasm->wasm_brains != nullptr);

    setup_variety_fixture(*mock);
    setup_variety_fixture(*wasm);

    // Generous margin past the >= 300 the spec asks for: the cranked (but
    // deliberately not instant) need drains, the Hunt chase, and a Hunter's
    // explore-appetite lease windows all need real ticks to resolve, not
    // just be theoretically reachable.
    constexpr int kTicks = 2000;
    for (int i = 0; i < kTicks; ++i) {
        tick_world(*mock, 1.0f / 30.0f);
        tick_world(*wasm, 1.0f / 30.0f);
    }

    CHECK(stats_of(*wasm).noiser_bugs == 0);  // no wasm errors anywhere in the run

    const std::vector<Command>& a = mock->command_log;
    const std::vector<Command>& b = wasm->command_log;
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());

    // Comparison strictness is unchanged from the first version of this
    // test: kind/actor/target/params/timing EXACT; point within 1e-4 solely
    // for potential fp-contract differences between native arm64 C++ and
    // wasm (see task-5-brief.md) -- integer-derived floats should in
    // practice be exact, and (see the report) every point comparison in this
    // run in fact was.
    std::set<int32_t> non_idle_activities;
    bool saw_shoot_command = false;
    bool saw_chat_command = false;

    for (size_t i = 0; i < a.size(); ++i) {
        INFO("command " << i << " kind=" << static_cast<int32_t>(a[i].kind));
        CHECK(a[i].kind == b[i].kind);
        CHECK(a[i].actor == b[i].actor);
        CHECK(a[i].target_id == b[i].target_id);
        CHECK(a[i].param_a == b[i].param_a);
        CHECK(a[i].param_b == b[i].param_b);
        CHECK(a[i].at_millis == b[i].at_millis);
        CHECK(std::abs(a[i].point.x - b[i].point.x) <= 1e-4f);
        CHECK(std::abs(a[i].point.y - b[i].point.y) <= 1e-4f);

        if (a[i].kind == CommandKind::SetBehavior &&
            a[i].param_a != static_cast<int32_t>(ActivityId::Idle)) {
            non_idle_activities.insert(a[i].param_a);
        }
        // The hunt shot is a TARGETED Attack (target_id = prey slot); combat's
        // untargeted re-pick form carries UINT32_MAX, and this fixture has no
        // hostiles anyway, so this cleanly identifies the hunter's shot.
        saw_shoot_command = saw_shoot_command ||
                            (a[i].kind == CommandKind::Attack && a[i].target_id != UINT32_MAX);
        saw_chat_command = saw_chat_command || (a[i].kind == CommandKind::Chat);
    }

    // Sanity guard, strengthened per review: a vacuous all-flat parity pass
    // (Roam/Buy/VisitTavern/Think, none of which needed the fixture above --
    // exactly what the first version of this test actually proved) must
    // fail this. >= 3 distinct non-Idle activities, AND each of the seven
    // non-Idle activities in kHeroActivities individually -- the fixture's
    // claimed all-8 coverage (see setup_variety_fixture's comment), asserted
    // one by one so fixture drift can't silently shrink it back down.
    CHECK(non_idle_activities.size() >= 3);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::Hunt)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::Chat)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::GoHome)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::Explore)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::Buy)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::VisitTavern)) == 1);
    CHECK(non_idle_activities.count(static_cast<int32_t>(ActivityId::Roam)) == 1);

    // The two new wire extensions (BL_CMD_SHOOT/BL_CMD_CHAT, decode_command)
    // are unverified if only the SetBehavior(Hunt)/SetBehavior(Chat)
    // ACTIVITY shows up -- that proves only that the activity was selected,
    // not that the new wire path was ever exercised end to end (a follow-up
    // command decoded WRONG, or never decoded at all, would still leave
    // SetBehavior(Hunt)/(Chat) in the log). These two checks are what close
    // that gap; the per-entry loop above already proved both sides agree on
    // every field of every such command, including target_id (the actual
    // command_arg -> target-slot mapping the wire extension exists for).
    CHECK(saw_shoot_command);
    CHECK(saw_chat_command);
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
