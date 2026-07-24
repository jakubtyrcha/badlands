// Task 4 (wasm-brain feature): wiring the wasm brain runtime into the sim.
// Most cases here load the REAL, shipping assets/brains/hero.wasm (Task 5's
// ported decision layer, scripts/brains/nim/hero.nim) -- the twin-brain
// parity test against the C++ reference lives in hero_brain_parity_tests.cpp;
// this file is about the wasm PLUMBING (load/spawn/tick/reinstantiate/combat
// pre-empt), not decision correctness. Cases that need a brain PINNED to
// all-Idle (so a test can assert on that alone) load
// game/tests/fixtures/idle_brain.wasm instead (scripts/brains/nim/idle_test.nim).
//
// apply_brain_decision (the shared decision-apply seam, town_brain.h) and
// decode_decision (the wire trust boundary, wasm_brain.h) are unit-tested
// directly against synthetic BrainDecisions/BlDecisionWires, no wasm involved
// -- they are exactly town_think's former tail plus the wire-decode step,
// generalized; the rest of this file exercises the wasm plumbing end to end
// through badlands::Sim.

#include "badlands_sim.hpp"
#include "command.h"
#include "duel_common.h"
#include "game_state.h"
#include "sim_internal.hpp"
#include "town_brain.h"
#include "wasm_brain.h"  // WasmBrainRuntime::instantiation_count, decode_decision (review-fix coverage)

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace badlands;
using namespace testfix;

namespace {

// Reads a binary wasm fixture, repo-root-relative like every other asset
// path in this codebase (add_test sets WORKING_DIRECTORY to the repo root,
// see CMakeLists.txt).
std::vector<uint8_t> read_wasm_file(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.data()), size));
    return bytes;
}

// The shipping brain artifact (LFS binary; scripts/brains/nim/hero.nim) --
// the real, ported decision layer (Task 5).
std::vector<uint8_t> read_hero_wasm() { return read_wasm_file("assets/brains/hero.wasm"); }

// Test-only fixture (LFS binary; scripts/brains/nim/idle_test.nim -- same
// export surface as hero.nim, but bl_tick always decides Idle/no-goal/
// no-command/no-pause, unconditionally): built by scripts/build_brains.sh
// alongside hero.wasm. What hero.wasm itself used to be before Task 5.
std::vector<uint8_t> read_idle_wasm() {
    return read_wasm_file("game/tests/fixtures/idle_brain.wasm");
}

// Test-only fixture (LFS binary; scripts/brains/nim/trap_test.nim -- same
// export surface as hero.nim, but bl_tick unconditionally traps): built by
// scripts/build_brains.sh alongside hero.wasm.
std::vector<uint8_t> read_trap_wasm() {
    return read_wasm_file("game/tests/fixtures/trap_brain.wasm");
}

BrainDesc wasm_desc(const std::vector<uint8_t>& bytes) {
    return BrainDesc{.wasm_bytes = bytes.data(), .wasm_len = bytes.size()};
}

// A minimal home-less hero (Archetype defaults to Hero), for the
// apply_brain_decision unit tests -- only Position/MoveTarget/
// HeroSimulationState matter there, so the rest of CharacterDesc is left at
// its zero default.
CharacterDesc bare_hero(float x, float z) {
    CharacterDesc d{};
    d.pos_x = x;
    d.pos_z = z;
    d.team = 0;
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    return d;
}

// Places one guild + recruits `n` heroes through the command path, exactly
// as determinism_tests.cpp's seed_town does (duplicated per that file's own
// fixture-independence convention) -- so the seed itself is part of the
// recorded/replayed log.
void seed_heroes(BadlandsGame* g, int n) {
    // Same coordinates determinism_tests.cpp's seed_town uses -- building
    // placement does not care about terrain, only unit movement does.
    Action place{ActionKind::PlaceBuilding, 0, -14.0f, -8.0f,
                static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    const int64_t guild = dispatch_into(*g, place);
    REQUIRE(guild >= 0);
    for (int i = 0; i < n; ++i) {
        Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(guild), 0.0f, 0.0f, 0, 0};
        REQUIRE(dispatch_into(*g, recruit) >= 0);
    }
}

bool same_command(const Command& a, const Command& b) {
    return a.kind == b.kind && a.actor == b.actor && a.target_id == b.target_id &&
          a.point == b.point && a.param_a == b.param_a && a.param_b == b.param_b &&
          a.at_millis == b.at_millis;
}

}  // namespace

// --- F.1: the idle-fixture wasm brain drives every hero, no bugs -----------

TEST_CASE("wasm: every hero stays Idle over 30 ticks with the idle fixture brain, no bugs") {
    std::vector<uint8_t> bytes = read_idle_wasm();
    Sim sim(wasm_desc(bytes));

    std::vector<uint32_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(sim.Spawn(mercenary(static_cast<float>(i) * 6.0f, kDuelGroundZ)));
    }
    for (int i = 0; i < 30; ++i) {
        sim.Tick(kTickDt);
    }

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 3);
    for (const CharacterState& r : rows) {
        CHECK(r.behavior == static_cast<int32_t>(ActivityId::Idle));
    }

    SimStats stats = sim.GetStats();
    CHECK(stats.noiser_bugs == 0);     // no wasm errors
    CHECK(stats.script_intents > 0);   // decisions were actually applied
}

// --- F.2: combat pre-empt still owns enemies, even with wasm loaded --------

TEST_CASE("wasm: combat pre-empt still owns enemies") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    Sim sim(wasm_desc(bytes));

    CharacterDesc merc = mercenary(-8.0f, kDuelGroundZ);
    CharacterDesc gob = goblin(8.0f, kDuelGroundZ);
    uint32_t merc_id = sim.Spawn(merc);
    sim.Spawn(gob);

    CharacterState survivor = run_duel(sim);

    CHECK(survivor.id == merc_id);
    CHECK(survivor.team == 0);
    CHECK(survivor.hp < survivor.max_hp);  // the goblin got its licks in

    // combat_preempt claims the mercenary's tick for as long as the goblin is
    // alive, so tick_wasm_brain is never reached for it during the duel --
    // whatever the loaded brain would otherwise decide (Idle or the real
    // hero decision layer) never gets a chance to interfere with combat.
    CHECK(sim.GetStats().noiser_bugs == 0);
}

// --- F.3: a garbage module reports a bug and heroes fall back to mock ------

TEST_CASE("wasm: garbage bytes report a bug and heroes fall back to mock behaviour") {
    // Not a wasm module at all (no \0asm magic) -- bh_load must fail to
    // compile it.
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0x10, 0x20};
    Sim sim(BrainDesc{.wasm_bytes = garbage, .wasm_len = sizeof(garbage)});

    CHECK(sim.GetStats().noiser_bugs > 0);  // the load failure was recorded

    uint32_t id = sim.Spawn(bare_hero(0.0f, kDuelGroundZ));
    for (int i = 0; i < 5; ++i) {
        sim.Tick(kTickDt);
    }

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == id);
    // The cheapest observable that mock town_think is genuinely driving
    // (not stalled): with the compiled default weights, Roam (weight 1.0,
    // always available) outscores Idle (0.5) from the very first tick, so a
    // freshly spawned, mock-driven hero leaves Idle almost immediately (a
    // wasm-load failure that left the entity silently doing nothing would
    // fail this).
    CHECK(rows[0].behavior != static_cast<int32_t>(ActivityId::Idle));
}

// --- Review fix: a trapped BhInstance is dropped and re-instantiated -------
// (brainhost.h: "a BhInstance that has trapped (BH_ERR_TRAP/BH_ERR_FUEL) is
// not reused"). Uses a real trapping module (game/tests/fixtures/
// trap_brain.wasm, scripts/brains/nim/trap_test.nim) rather than a synthetic
// error code, so this exercises bh_tick's actual BH_ERR_TRAP return, not an
// assumption about it.

TEST_CASE("wasm: a trapping module reports a bug every tick and never crashes the sim") {
    std::vector<uint8_t> bytes = read_trap_wasm();
    Sim sim(wasm_desc(bytes));

    constexpr int kHeroes = 2;
    constexpr int kTicks = 10;
    std::vector<uint32_t> ids;
    for (int i = 0; i < kHeroes; ++i) {
        ids.push_back(sim.Spawn(mercenary(static_cast<float>(i) * 6.0f, kDuelGroundZ)));
    }
    for (int i = 0; i < kTicks; ++i) {
        sim.Tick(kTickDt);
    }

    // No crash, and every spawned hero is still a valid, healthy row --
    // combat/movement/needs keep running untouched around a permanently
    // failing brain.
    auto rows = sim.Characters();
    REQUIRE(rows.size() == static_cast<size_t>(kHeroes));
    for (const CharacterState& r : rows) {
        CHECK(r.hp > 0.0f);
    }

    // bh_spawn never traps in this fixture (only bl_tick does), so every
    // tick's bh_tick call is the sole failure per hero: exactly
    // kHeroes * kTicks accrued bugs -- "one per hero tick" and, since that
    // count keeps accruing at the full rate through the last tick, proof
    // that re-instantiation kept succeeding throughout (a wedged/nulled-out
    // runtime would have silently stopped producing "wasm_tick" bugs and
    // fallen back to mock instead).
    CHECK(sim.GetStats().noiser_bugs == static_cast<uint32_t>(kHeroes * kTicks));
}

TEST_CASE("wasm: each trap gets a freshly instantiated BhInstance, not the trapped one") {
    std::vector<uint8_t> bytes = read_trap_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);
    CHECK(g->wasm_brains->instantiation_count == 1);  // create()'s own instantiation

    spawn_into(*g, mercenary(0.0f, kDuelGroundZ));  // Town-kind, no enemy: ticks the wasm brain

    tick_world(*g, 1.0f / 30.0f);
    REQUIRE(g->wasm_brains != nullptr);  // re-instantiation succeeded, not nulled out
    const uint32_t after_tick1 = g->wasm_brains->instantiation_count;
    CHECK(after_tick1 > 1);  // a fresh instance replaced the one that just trapped

    tick_world(*g, 1.0f / 30.0f);
    REQUIRE(g->wasm_brains != nullptr);
    const uint32_t after_tick2 = g->wasm_brains->instantiation_count;
    CHECK(after_tick2 > after_tick1);  // a SECOND, distinct re-instantiation happened
}

// --- F.4: apply_brain_decision, unit-tested directly (no wasm) -------------

TEST_CASE("apply_brain_decision: commit writes SetBehavior + MoveTo to the log") {
    auto g = make_world(nullptr);
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.activity = ActivityId::Roam;
    d.goal = self_pos + glm::vec2{5.0f, 0.0f};
    apply_brain_decision(*g, slot, self_pos, d);
    apply_commands(*g);

    REQUIRE(g->command_log.size() == 2);
    CHECK(g->command_log[0].kind == CommandKind::SetBehavior);
    CHECK(g->command_log[0].param_a == static_cast<int32_t>(ActivityId::Roam));
    CHECK(g->command_log[1].kind == CommandKind::MoveTo);
    CHECK(g->command_log[1].point == d.goal);
}

TEST_CASE("apply_brain_decision: pause-start writes Think + a single hold MoveTo") {
    auto g = make_world(nullptr);
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.pause = true;
    d.pause_duration_millis = 500;
    apply_brain_decision(*g, slot, self_pos, d);
    apply_commands(*g);

    REQUIRE(g->command_log.size() == 2);
    CHECK(g->command_log[0].kind == CommandKind::SetBehavior);
    CHECK(g->command_log[0].param_a == static_cast<int32_t>(ActivityId::Think));
    CHECK(g->command_log[0].param_b == 500);
    CHECK(g->command_log[1].kind == CommandKind::MoveTo);
    CHECK(g->command_log[1].point == self_pos);
}

TEST_CASE("apply_brain_decision: pause-continue enqueues nothing") {
    auto g = make_world(nullptr);
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.pause = true;
    d.pause_duration_millis = 0;
    apply_brain_decision(*g, slot, self_pos, d);
    CHECK(g->command_queue.empty());
    apply_commands(*g);
    CHECK(g->command_log.empty());
}

TEST_CASE("apply_brain_decision: follow_up_on_arrival gates on distance to the goal") {
    auto g = make_world(nullptr);
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    // Far target, gated: the follow-up must NOT appear yet.
    {
        BrainDecision d;
        d.activity = ActivityId::VisitTavern;
        d.goal = self_pos + glm::vec2{20.0f, 0.0f};  // well past kEntranceRadius
        d.follow_up = Command{CommandKind::EnterBuilding, slot};
        d.follow_up_on_arrival = true;
        apply_brain_decision(*g, slot, self_pos, d);
        apply_commands(*g);
        for (const Command& c : g->command_log) {
            CHECK(c.kind != CommandKind::EnterBuilding);
        }
    }

    // Near target, gated: the follow-up fires.
    {
        BrainDecision d;
        d.activity = ActivityId::VisitTavern;
        d.goal = self_pos;  // distance 0 <= kEntranceRadius
        d.follow_up = Command{CommandKind::EnterBuilding, slot};
        d.follow_up_on_arrival = true;
        apply_brain_decision(*g, slot, self_pos, d);
        apply_commands(*g);
        bool found = false;
        for (const Command& c : g->command_log) {
            found = found || (c.kind == CommandKind::EnterBuilding && c.actor == slot);
        }
        CHECK(found);
    }

    // Far target, UNgated (follow_up_on_arrival = false): fires regardless of
    // distance -- act_chat/act_hunt's semantics (a shot/greeting emitted the
    // instant it becomes valid, not on arrival at the walk target).
    {
        BrainDecision d;
        d.activity = ActivityId::Hunt;
        d.goal = self_pos + glm::vec2{20.0f, 0.0f};
        d.follow_up = Command{CommandKind::Shoot, slot, 0};
        d.follow_up_on_arrival = false;
        apply_brain_decision(*g, slot, self_pos, d);
        apply_commands(*g);
        bool found = false;
        for (const Command& c : g->command_log) {
            found = found || (c.kind == CommandKind::Shoot && c.actor == slot);
        }
        CHECK(found);
    }
}

// --- Review fix: decode_decision rejects a non-finite goal (the wire trust
// boundary) -----------------------------------------------------------------
// A synthetic BlDecisionWire, no wasm module involved -- exercises the same
// seam a real (buggy/adversarial) guest's bl_out_buf write would hit.

TEST_CASE("decode_decision: a non-finite goal is rejected, not decoded") {
    auto g = make_world(nullptr);
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.activity_id = static_cast<int32_t>(ActivityId::Roam);
    wire.goal_kind = 1;
    wire.goal_x = std::numeric_limits<float>::quiet_NaN();
    wire.goal_z = 0.0f;

    REQUIRE(g->noiser_bugs == 0);
    const std::optional<BrainDecision> decision = decode_decision(*g, wire, slot, self_pos);
    CHECK(!decision.has_value());
    CHECK(g->noiser_bugs == 1);  // report_bug'd once
    CHECK(g->command_queue.empty());  // no follow-up MoveTo enqueued

    apply_commands(*g);
    for (const Command& c : g->command_log) {
        CHECK(c.kind != CommandKind::MoveTo);
    }
}

// --- F.5: determinism with wasm loaded --------------------------------------

TEST_CASE("wasm: two identical runs produce identical command logs and character snapshots") {
    std::vector<uint8_t> bytes = read_hero_wasm();

    auto a = make_world(wasm_desc(bytes));
    auto b = make_world(wasm_desc(bytes));
    REQUIRE(a->wasm_brains != nullptr);
    REQUIRE(b->wasm_brains != nullptr);
    seed_heroes(a.get(), 3);
    seed_heroes(b.get(), 3);

    constexpr int kTicks = 150;
    for (int i = 0; i < kTicks; ++i) {
        tick_world(*a, 1.0f / 30.0f);
        tick_world(*b, 1.0f / 30.0f);
    }

    REQUIRE(a->command_log.size() == b->command_log.size());
    REQUIRE(!a->command_log.empty());
    for (size_t i = 0; i < a->command_log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(a->command_log[i], b->command_log[i]));
    }

    const std::vector<CharacterState> ca = characters_of(*a);
    const std::vector<CharacterState> cb = characters_of(*b);
    REQUIRE(ca.size() == cb.size());
    for (size_t i = 0; i < ca.size(); ++i) {
        INFO("character row " << i);
        CHECK(ca[i].pos_x == cb[i].pos_x);
        CHECK(ca[i].pos_z == cb[i].pos_z);
        CHECK(ca[i].behavior == cb[i].behavior);
        CHECK(ca[i].fatigue == cb[i].fatigue);
        CHECK(ca[i].content == cb[i].content);
    }
}
