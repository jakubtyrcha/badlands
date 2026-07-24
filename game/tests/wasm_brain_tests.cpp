// Wasm hero brain wire v2 (the intention contract, docs/design/
// intention-contract.html): a brain suggests ONE intention per wake, the
// engine (game/src/intention.h) validates/executes/tracks it and decides
// when to wake the brain again. This file exercises the WASM PLUMBING --
// pack_view_wire (statuses/events/self land in the wire correctly),
// decode_suggestion (the wire trust boundary: each BL_INT_* maps, malformed
// wires are rejected, BL_INT_USE_SKILL warns+ignores), and tick_wasm_brain
// end to end through badlands::Sim/tick_world (spawn/tick, combat pre-empt,
// the wake gate, determinism). Decision CORRECTNESS is not a parity target
// (no C++ reference brain exists anymore -- "behavior-in-spirit" is the bar,
// see the design doc's §3); cases that need a brain pinned to a fixed
// decision load game/tests/fixtures/idle_brain.wasm
// (scripts/brains/nim/idle_test.nim) instead of the real, shipping
// assets/brains/hero.wasm.

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "duel_common.h"
#include "game_state.h"
#include "hero_perception.h"  // observe_hero/weights_for/WorldView/ActivityWeights
#include "intention.h"
#include "sim_internal.hpp"
#include "wasm_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
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
// the real, ported hero decision layer.
std::vector<uint8_t> read_hero_wasm() { return read_wasm_file("assets/brains/hero.wasm"); }

// Test-only fixture (LFS binary; scripts/brains/nim/idle_test.nim -- same
// export surface as hero.nim, but bl_tick always suggests BL_INT_IDLE/
// ActivityId::Idle, unconditionally): built by scripts/build_brains.sh
// alongside hero.wasm.
std::vector<uint8_t> read_idle_wasm() {
    return read_wasm_file("game/tests/fixtures/idle_brain.wasm");
}

BrainDesc wasm_desc(const std::vector<uint8_t>& bytes) {
    return BrainDesc{.wasm_bytes = bytes.data(), .wasm_len = bytes.size()};
}

// A minimal home-less hero (Archetype defaults to Hero), for the
// pack_view_wire/decode_suggestion fixtures -- only the components those
// functions actually read matter, so the rest of CharacterDesc is left at
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

// --- wasm plumbing smokes (fixed-decision brains) ---------------------------

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

    // Decisions were actually applied: every adopted intention logs a
    // SetBehavior command (apply_intention, intention.cpp), so a nonempty
    // command log is the "a decision landed" signal.
    CHECK_FALSE(sim.CommandLog().empty());
}

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
    // alive, so should_wake/tick_wasm_brain are never reached for it during
    // the duel -- whatever the loaded brain would otherwise suggest never
    // gets a chance to interfere with combat.
}

// --- pack_view_wire: the view side of the wire trust boundary --------------

TEST_CASE("pack_view_wire: statuses assemble from Chatting/MeleeLock/InsideBuilding",
          "[wasm_brain]") {
    auto g = make_world(BrainDesc{});  // host-only: no wasm module needed

    SECTION("Chatting") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<ChattingState>(e, /*partner_slot=*/7u, /*remaining=*/2.5f);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_CHATTING);
        CHECK(wire.statuses[0].remaining_millis == 2500);
    }

    SECTION("MeleeLock") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<MeleeLock>(e);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_MELEE_LOCKED);
        CHECK(wire.statuses[0].remaining_millis == 0);  // indefinite
    }

    SECTION("InsideBuilding") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<InsideBuilding>(e, /*building_id=*/0, /*purpose=*/0);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_INSIDE_BUILDING);
        CHECK(wire.statuses[0].remaining_millis == 0);  // indefinite
    }

    SECTION("none of the three -> status_count 0") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        CHECK(wire.status_count == 0);
    }
}

TEST_CASE("pack_view_wire: events copy 1:1 from the EventInbox", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];

    InboxEvent ev;
    ev.kind = InboxEventKind::DamageTaken;
    ev.source_slot = 3;
    ev.param = 5.0f;
    push_inbox_event(*g, e, ev);  // stamps at_millis/ttl_millis from game state

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    REQUIRE(wire.event_count == 1);
    CHECK(wire.events[0].kind == BL_EV_DAMAGE_TAKEN);
    CHECK(wire.events[0].source_slot == 3);
    CHECK(wire.events[0].param == Catch::Approx(5.0f));
    CHECK(wire.events[0].at_millis == g->world_millis);
    CHECK(wire.events[0].ttl_millis == kInboxTtlMillis);
}

TEST_CASE("pack_view_wire: self carries the CurrentIntention summary", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];

    CurrentIntention& ci = g->registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = 4321;

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    CHECK(wire.self.intention_kind == BL_INT_IDLE);
    CHECK(wire.self.intention_wake_at == 4321);
}

// --- decode_suggestion: the suggestion side of the wire trust boundary ------

TEST_CASE("decode_suggestion: each BL_INT_* kind maps onto the matching IntentionKind",
          "[wasm_brain]") {
    struct Case {
        int32_t wire_kind;
        IntentionKind expect;
    };
    const Case cases[] = {
        {BL_INT_NONE, IntentionKind::None},           {BL_INT_MOVE_TO, IntentionKind::MoveTo},
        {BL_INT_ATTACK, IntentionKind::Attack},        {BL_INT_SHOOT, IntentionKind::Shoot},
        {BL_INT_ENTER, IntentionKind::Enter},          {BL_INT_ENTER_HOME, IntentionKind::EnterHome},
        {BL_INT_BUY, IntentionKind::Buy},              {BL_INT_CHAT, IntentionKind::Chat},
        {BL_INT_IDLE, IntentionKind::Idle},
    };
    for (const Case& c : cases) {
        INFO("wire_kind " << c.wire_kind);
        BlSuggestionWire wire{};
        wire.intention_kind = c.wire_kind;
        wire.target_slot = 5;
        wire.arg = 2;
        wire.duration_millis = 100;
        wire.idle_hint_millis = 200;
        wire.point_x = 1.0f;
        wire.point_z = 2.0f;
        wire.activity_label = static_cast<int32_t>(ActivityId::Roam);

        const std::optional<Intention> intent = decode_suggestion(wire, /*slot=*/0);
        REQUIRE(intent.has_value());
        CHECK(intent->kind == c.expect);
        CHECK(intent->target_slot == 5);
        CHECK(intent->arg == 2);
        CHECK(intent->duration_millis == 100);
        CHECK(intent->idle_hint_millis == 200);
        CHECK(intent->point.x == Catch::Approx(1.0f));
        CHECK(intent->point.y == Catch::Approx(2.0f));
        CHECK(intent->activity_label == static_cast<int32_t>(ActivityId::Roam));
    }
}

TEST_CASE("decode_suggestion: BL_INT_USE_SKILL is reserved -- decodes to IntentionKind::None, "
         "not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = BL_INT_USE_SKILL;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());  // well-formed -- not the malformed/FATAL path
    CHECK(intent->kind == IntentionKind::None);
}

TEST_CASE("decode_suggestion: malformed wires are rejected", "[wasm_brain]") {
    // intention_kind out of range and activity_label out of range are NOT
    // here (Fix 5): they are unknown VOCABULARY, not malformed -- see the
    // "forward-compatible vocabulary decode" cases below.
    SECTION("non-finite point_x") {
        BlSuggestionWire wire{};
        wire.point_x = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("non-finite point_z") {
        BlSuggestionWire wire{};
        wire.point_z = std::numeric_limits<float>::infinity();
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("negative duration_millis") {
        BlSuggestionWire wire{};
        wire.duration_millis = -1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("duration_millis beyond INT32_MAX (would truncate through Command::param_b)") {
        BlSuggestionWire wire{};
        wire.intention_kind = BL_INT_IDLE;
        wire.duration_millis = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("negative idle_hint_millis") {
        BlSuggestionWire wire{};
        wire.idle_hint_millis = -1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("idle_hint_millis beyond INT32_MAX") {
        BlSuggestionWire wire{};
        wire.idle_hint_millis = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
}

// --- Fix 5: forward-compatible vocabulary decode ----------------------------
// An intention_kind/activity_label this build doesn't recognize is UNKNOWN
// VOCABULARY (e.g. a newer guest talking to an older host), not corruption --
// decode as None/-1 with a warning, never reject. Only corruption-shaped
// wires (non-finite point floats, negative/overflowing duration/hint) stay
// FATAL, pinned by the "malformed wires are rejected" test above.

TEST_CASE("decode_suggestion: an unrecognized intention_kind decodes as None, not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = 99;  // outside [BL_INT_NONE, BL_INT_USE_SKILL]
    wire.point_x = 1.0f;
    wire.point_z = 2.0f;
    wire.target_slot = 5;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());  // well-formed -- not the malformed/FATAL path
    CHECK(intent->kind == IntentionKind::None);
    // The rest of the wire still decodes normally -- only the kind is unknown.
    CHECK(intent->point.x == Catch::Approx(1.0f));
    CHECK(intent->target_slot == 5);
}

TEST_CASE("decode_suggestion: a negative intention_kind decodes as None, not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = -1;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());
    CHECK(intent->kind == IntentionKind::None);
}

TEST_CASE("decode_suggestion: an out-of-range activity_label clamps to -1, not rejected",
          "[wasm_brain]") {
    SECTION("too high") {
        BlSuggestionWire wire{};
        wire.activity_label = kActivityCount;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
    SECTION("below -1") {
        BlSuggestionWire wire{};
        wire.activity_label = -2;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
    SECTION("-1 itself is the valid inspection-only sentinel, not clamped/warned") {
        BlSuggestionWire wire{};
        wire.activity_label = -1;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
}

TEST_CASE("decode_suggestion + apply_intention: the idle hint plumbs through to "
         "CurrentIntention.wake_at_millis",
          "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];
    g->world_millis = 1000;

    BlSuggestionWire wire{};
    wire.intention_kind = BL_INT_ATTACK;  // any non-Idle kind: idle_hint_millis (not
                                          // duration_millis) carries the schedule
    wire.idle_hint_millis = 750;

    const std::optional<Intention> intent = decode_suggestion(wire, slot);
    REQUIRE(intent.has_value());
    CHECK(apply_intention(*g, slot, *intent));
    CHECK(g->registry.get<CurrentIntention>(e).wake_at_millis == 1750);
}

// --- integration: a real, shipping wasm brain driving a live sim -----------

TEST_CASE("wasm: a town world ticks, heroes act (MoveTo/SetBehavior reach the log)") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    seed_heroes(g.get(), 3);
    for (int i = 0; i < 150; ++i) {
        tick_world(*g, 1.0f / 30.0f);
    }

    bool saw_move = false;
    bool saw_set_behavior = false;
    for (const Command& c : g->command_log) {
        saw_move = saw_move || c.kind == CommandKind::MoveTo;
        saw_set_behavior = saw_set_behavior || c.kind == CommandKind::SetBehavior;
    }
    CHECK(saw_move);
    CHECK(saw_set_behavior);
}

TEST_CASE("wasm: a sleeping hero (long hint) is woken by damage and re-decides within a tick") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    uint32_t slot = spawn_into(*g, MercenaryDesc(0.0f, kCastleSpawnZ));
    entt::entity e = g->slots[slot];

    // One real wake: let the hero actually think (bh_spawn + a genuine
    // suggestion) so CurrentIntention reflects what react() produced.
    tick_world(*g, 1.0f / 30.0f);
    REQUIRE(g->registry.get<CurrentIntention>(e).last_think_millis == g->world_millis);

    // Force it deep asleep: an artificially far wake_at_millis, well beyond
    // any idle hint hero.nim could actually draw -- deterministic,
    // independent of the hint's real (randomized, bounded) value.
    CurrentIntention& ci = g->registry.get<CurrentIntention>(e);
    ci.wake_at_millis = g->world_millis + 60'000;
    REQUIRE_FALSE(should_wake(*g, e));

    const int64_t last_think_before = ci.last_think_millis;
    for (int i = 0; i < 5; ++i) {
        tick_world(*g, 1.0f / 30.0f);  // asleep: no re-think while nothing happens
    }
    CHECK(g->registry.get<CurrentIntention>(e).last_think_millis == last_think_before);

    // Inject a hit -- the same DamageTaken choke point real combat uses
    // (emit_char_hit, game_state.h) -- a guaranteed-wake event
    // (docs/design/intention-contract.html §2).
    emit_char_hit(*g, /*actor_slot=*/UINT32_MAX, slot, 3.0f,
                 g->registry.get<Health>(e).hp - 3.0f, g->registry.get<Position>(e).pos);
    CHECK(should_wake(*g, e));

    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->registry.get<CurrentIntention>(e).last_think_millis == g->world_millis);
}

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
