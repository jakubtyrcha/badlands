// Task 4 (wasm-brain feature), amended by Task 7 (fail-fast policy): wiring
// the wasm brain runtime into the sim. Most cases here load the REAL,
// shipping assets/brains/hero.wasm (Task 5's ported decision layer,
// scripts/brains/nim/hero.nim) -- the twin-brain parity test against the
// C++ reference lives in hero_brain_parity_tests.cpp; this file is about the
// wasm PLUMBING (load/spawn/tick/combat pre-empt), not decision correctness.
// Cases that need a brain PINNED to all-Idle (so a test can assert on that
// alone) load game/tests/fixtures/idle_brain.wasm instead
// (scripts/brains/nim/idle_test.nim).
//
// A wasm-brain failure is FATAL under this project's policy (wasm_brain.h's
// policy note; docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md's
// Runtime section), so there is nothing left to test at the sim level about a
// trap or a load failure surviving gracefully -- that coverage moved to the
// brainhost crate's real_trap_wasm_traps test (src/crates/brainhost/src/
// lib.rs), which pins the Nim-panic -> wasm-trap -> BH_ERR_TRAP chain the
// trap_brain.wasm fixture exists for; this file no longer reads that fixture.
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
#include "wasm_brain.h"  // decode_decision (review-fix coverage)

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

// --- F.4: apply_brain_decision, unit-tested directly (no wasm) -------------

TEST_CASE("apply_brain_decision: commit writes SetBehavior + MoveTo to the log") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.activity = ActivityId::Roam;
    d.goal = self_pos + glm::vec2{5.0f, 0.0f};
    CHECK(apply_brain_decision(*g, slot, self_pos, d));  // commit: applied
    apply_commands(*g);

    REQUIRE(g->command_log.size() == 2);
    CHECK(g->command_log[0].kind == CommandKind::SetBehavior);
    CHECK(g->command_log[0].param_a == static_cast<int32_t>(ActivityId::Roam));
    CHECK(g->command_log[1].kind == CommandKind::MoveTo);
    CHECK(g->command_log[1].point == d.goal);
}

TEST_CASE("apply_brain_decision: pause-start writes Think + a single hold MoveTo") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.pause = true;
    d.pause_duration_millis = 500;
    CHECK(apply_brain_decision(*g, slot, self_pos, d));  // pause-START: applied
    apply_commands(*g);

    REQUIRE(g->command_log.size() == 2);
    CHECK(g->command_log[0].kind == CommandKind::SetBehavior);
    CHECK(g->command_log[0].param_a == static_cast<int32_t>(ActivityId::Think));
    CHECK(g->command_log[0].param_b == 500);
    CHECK(g->command_log[1].kind == CommandKind::MoveTo);
    CHECK(g->command_log[1].point == self_pos);
}

TEST_CASE("apply_brain_decision: pause-continue enqueues nothing") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BrainDecision d;
    d.pause = true;
    d.pause_duration_millis = 0;
    CHECK_FALSE(apply_brain_decision(*g, slot, self_pos, d));  // pause-CONTINUE: not applied
    CHECK(g->command_queue.empty());
    apply_commands(*g);
    CHECK(g->command_log.empty());
}

TEST_CASE("apply_brain_decision: follow_up_on_arrival gates on distance to the goal") {
    auto g = make_world(BrainDesc{});
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
        d.follow_up = Command{CommandKind::Attack, slot, 0};  // targeted attack (the hunt shot)
        d.follow_up_on_arrival = false;
        apply_brain_decision(*g, slot, self_pos, d);
        apply_commands(*g);
        bool found = false;
        for (const Command& c : g->command_log) {
            found = found || (c.kind == CommandKind::Attack && c.actor == slot && c.target_id == 0);
        }
        CHECK(found);
    }
}

// --- Review fix: decode_decision rejects malformed wires (the wire trust
// boundary) -----------------------------------------------------------------
// Synthetic BlDecisionWires, no wasm module involved -- exercise the same
// seam a real (buggy/adversarial) guest's bl_out_buf write would hit.
// decode_decision is a pure function (no report_bug/game-state side effect --
// under the fail-fast policy it is the CALLER, tick_wasm_brain, that
// escalates a nullopt to brain_fatal), so these assert only on the return
// value.

TEST_CASE("decode_decision: a non-finite goal is rejected, not decoded") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.activity_id = static_cast<int32_t>(ActivityId::Roam);
    wire.goal_kind = 1;
    wire.goal_x = std::numeric_limits<float>::quiet_NaN();
    wire.goal_z = 0.0f;

    const std::optional<BrainDecision> decision = decode_decision(*g, wire, slot, self_pos);
    CHECK(!decision.has_value());
    CHECK(g->command_queue.empty());  // no follow-up MoveTo enqueued

    apply_commands(*g);
    for (const Command& c : g->command_log) {
        CHECK(c.kind != CommandKind::MoveTo);
    }
}

// Task 7 review finding: pause_kind/pause_duration_millis were unvalidated --
// four cases, each isolating exactly one violation against an otherwise
// zero-initialized (so trivially valid: activity_id=0/Idle, goal_kind=0/none)
// wire.

TEST_CASE("decode_decision: pause_kind outside {0,1,2} is rejected") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.pause_kind = 3;

    CHECK(!decode_decision(*g, wire, slot, self_pos).has_value());
}

TEST_CASE("decode_decision: pause_kind==1 (start) with duration <= 0 is rejected") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.pause_kind = 1;
    wire.pause_duration_millis = 0;

    CHECK(!decode_decision(*g, wire, slot, self_pos).has_value());
}

TEST_CASE("decode_decision: pause_kind==1 (start) with duration beyond think_max_millis is "
         "rejected") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.pause_kind = 1;
    wire.pause_duration_millis = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    // This also bounds the int32_t truncation enqueue_set_behavior applies
    // (command.cpp) -- confirm the case under test actually exceeds the
    // policy bound, not just INT32_MAX in the abstract.
    REQUIRE(wire.pause_duration_millis > g->factors.hero.think_max_millis);

    CHECK(!decode_decision(*g, wire, slot, self_pos).has_value());
}

TEST_CASE("decode_decision: pause_kind==2 (continue) with nonzero duration is rejected") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    const glm::vec2 self_pos = g->registry.get<Position>(g->slots[slot]).pos;

    BlDecisionWire wire{};
    wire.pause_kind = 2;
    wire.pause_duration_millis = 17;

    CHECK(!decode_decision(*g, wire, slot, self_pos).has_value());
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
