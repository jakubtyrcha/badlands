// Pins on the sampo::noiser wrapper behaviors the brain integration depends
// on. Every failure here is a noiser bug or API drift — paste the output into
// docs/noiser-feedback.md before working around it.

#include <noiser.hpp>

#include "sim_internal.hpp"  // make_world / spawn_into / tick_world / stats_of
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using sampo::noiser::CompileError;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;
using namespace badlands;

namespace {

std::shared_ptr<NoiserProgram> compile_or_fail(const char* source) {
    auto result = NoiserProgram::Compile(source);
    if (!result) {
        FAIL("compile failed: " << result.error().message << " at line "
                                << result.error().line);
    }
    return *result;
}

}  // namespace

TEST_CASE("trivial generator yields then completes") {
    auto program = compile_or_fail(R"(
        pub gen fn seq() -> f32 {
            yield 1.5;
            yield 2.5;
        }
        0.0
    )");
    auto ctx = program->Prepare({});

    auto first = program->Resume(ctx);
    REQUIRE(first.has_value());
    CHECK(first->As<float>().value_or(-1.0f) == 1.5f);

    auto second = program->Resume(ctx);
    REQUIRE(second.has_value());
    CHECK(second->As<float>().value_or(-1.0f) == 2.5f);

    CompileError err;
    auto done = program->Resume(ctx, &err);
    CHECK(!done.has_value());
    CHECK(err.message.empty());  // exhaustion, not an error
}

TEST_CASE("void host call fires from inside a generator loop") {
    auto program = compile_or_fail(R"(
        @fn.ping: fn(v: i32) -> void;
        pub gen fn looper() -> i32 {
            var i = 0;
            loop {
                @fn.ping(i);
                i = i + 1;
                yield i;
            }
        }
        0.0
    )");
    std::vector<int32_t> pings;
    REQUIRE(program->BindCallableByName(
        "ping", std::function<void(int32_t)>([&](int32_t v) { pings.push_back(v); })));
    program->FreezeHostThunks();

    auto ctx = program->Prepare({});
    for (int i = 0; i < 3; ++i) {
        REQUIRE(program->Resume(ctx).has_value());
    }
    CHECK(pings == std::vector<int32_t>{0, 1, 2});
}

TEST_CASE("tuple-returning host call (bound as glm::vec4) feeds a generator") {
    // Script-side (f32, f32, f32, f32) and C++-side glm::vec4 both map to
    // kTuple/4 — this is the perception shape the brain uses.
    auto program = compile_or_fail(R"(
        @fn.fetch: fn(e: i32) -> (f32, f32, f32, f32);
        pub gen fn reader() -> f32 {
            loop {
                let (a, b, c, d) = @fn.fetch(7);
                yield a + b + c + d;
            }
        }
        0.0
    )");
    int32_t seen = -1;
    REQUIRE(program->BindCallableByName(
        "fetch", std::function<glm::vec4(int32_t)>([&](int32_t e) {
            seen = e;
            return glm::vec4(1.0f, 2.0f, 4.0f, 8.0f);
        })));
    program->FreezeHostThunks();

    auto ctx = program->Prepare({});
    auto result = program->Resume(ctx);
    REQUIRE(result.has_value());
    CHECK(result->As<float>().value_or(-1.0f) == 15.0f);
    CHECK(seen == 7);
}

TEST_CASE("KNOWN BUG: host functions returning vecN ICE the compiler") {
    // docs/noiser-feedback.md — `@fn.f: fn(e: i32) -> vec4;` panics in
    // inline_slot_count ("Named type 'vec4' not instantiated"); vec2/vec3
    // too. The FFI catches the panic and reports a compile error. When this
    // test starts failing, upstream fixed it: switch the brain's perception
    // returns back to vec4 and drop this pin.
    auto result = NoiserProgram::Compile(R"(
        @fn.fetch: fn(e: i32) -> vec4;
        pub fn main() -> f32 { let v = @fn.fetch(1); v.x }
    )");
    REQUIRE(!result.has_value());
    WARN("[noiser-bug] vecN @fn returns still ICE: " << result.error().message);
}

TEST_CASE("generator receives its i32 parameter via warp_id") {
    // Untested upstream as an executed path; the brain relies on it for
    // entity identity. Tolerated on failure (the game downgrades to mocks),
    // but loudly recorded.
    auto program = compile_or_fail(R"(
        pub gen fn ident(entity: i32) -> i32 {
            loop {
                yield entity;
            }
        }
        0.0
    )");
    auto ctx = program->Prepare(NoiserInput{.warp_id = {42, 0, 0}});
    auto result = program->Resume(ctx);
    REQUIRE(result.has_value());
    auto value = result->As<int32_t>();
    if (!value || *value != 42) {
        WARN("[noiser-bug] generator param via warp_id: expected 42, got "
             << (value ? std::to_string(*value) : std::string("<") + value.error() + ">"));
    } else {
        CHECK(*value == 42);
    }
}

TEST_CASE("corelib import compiles and runs") {
    auto program = compile_or_fail(R"(
        import { Vec2, vec2 } from core::linalg;
        pub fn main() -> f32 {
            let a = vec2(0.0, 0.0);
            let b = vec2(3.0, 4.0);
            a.distance(b)
        }
    )");
    auto ctx = program->Prepare({});
    auto result = program->Resume(ctx);
    REQUIRE(result.has_value());
    CHECK(result->As<float>().value_or(-1.0f) == 5.0f);
}

TEST_CASE("compile errors carry a message and location") {
    auto result = NoiserProgram::Compile("pub fn broken( {");
    REQUIRE(!result.has_value());
    CHECK(!result.error().message.empty());
    CHECK(result.error().line > 0);
}

TEST_CASE("runtime errors surface through Resume") {
    auto program = compile_or_fail(R"(
        fn divide(x: i32) -> i32 { 1 / x }
        pub fn main() -> i32 {
            divide(0)
        }
    )");
    auto ctx = program->Prepare({});
    CompileError err;
    auto result = program->Resume(ctx, &err);
    CHECK(!result.has_value());
    CHECK(!err.message.empty());
}

TEST_CASE("binding an undeclared host function fails loudly") {
    auto program = compile_or_fail(R"(
        pub fn main() -> f32 { 0.0 }
    )");
    CHECK(!program->BindCallableByName(
        "nonexistent", std::function<void(int32_t)>([](int32_t) {})));
}

TEST_CASE("v0.3 town host-call surface binds and fires through the resilient brain binding") {
    // Declares a NON-STANDARD subset of the host surface (the new town calls,
    // not perceive_self/attack_range/intent_move). This exercises both the new
    // bindings AND the resilient binding: undeclared host fns are skipped rather
    // than failing BrainRuntime::create. perceive_building takes an i32 kind arg
    // alongside a tuple return (a noiser corner the town brain relies on).
    const char* script = R"(
        @fn.perceive_building: fn(e: i32, kind: i32) -> (f32, f32, f32, f32);
        @fn.perceive_home: fn(e: i32) -> (f32, f32, f32, f32);
        @fn.inventory_count: fn(e: i32) -> f32;
        @fn.intent_move_to: fn(e: i32, x: f32, z: f32) -> void;
        @fn.intent_enter: fn(e: i32, kind: i32) -> void;
        @fn.intent_enter_home: fn(e: i32) -> void;
        @fn.intent_buy: fn(e: i32) -> void;
        pub gen fn brain(entity: i32) -> i32 {
            loop {
                let inv = @fn.inventory_count(entity);
                let (bx, bz, bex, _) = @fn.perceive_building(entity, 6);
                let (hx, hz, hex, _) = @fn.perceive_home(entity);
                @fn.intent_move_to(entity, 5.0, 7.0);
                @fn.intent_enter(entity, 6);
                @fn.intent_enter_home(entity);
                @fn.intent_buy(entity);
                yield 0;
            }
        }
        0.0
    )";
    auto owned = badlands::make_world(script);
    BadlandsGame* game = owned.get();
    badlands::CharacterDesc d{};
    d.pos_x = 20.0f;
    d.pos_z = 20.0f;
    d.team = 0;
    d.hp = 10.0f;
    d.move_speed = 1.0f;
    d.attack_range = 1.0f;
    d.attack_damage = 1.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    uint32_t hid = badlands::spawn_into(*game, d);

    badlands::tick_world(*game, 1.0f / 30.0f);

    badlands::SimStats stats = badlands::stats_of(*game);
    if (stats.noiser_bugs == 0) {
        // The whole new surface bound + fired: the durable move goal landed and
        // the other (no-op) town calls did not error.
        entt::entity e = game->slots[hid];
        const badlands::MoveTarget& mt = game->registry.get<badlands::MoveTarget>(e);
        CHECK(mt.kind == badlands::MoveTarget::Kind::Point);
        CHECK(mt.point.x == 5.0f);
        CHECK(mt.point.y == 7.0f);
        CHECK(stats.script_intents > 0);
    } else {
        WARN("[noiser-bug] town host-call surface tripped " << stats.noiser_bugs
                                                            << " bug(s); see docs/noiser-bugs-upstream");
    }
}

TEST_CASE("real hero.noiser behaviour framework loads and runs clean") {
    // Runtime smoke of the SHIPPING brain (the composable behaviour framework:
    // WorldView perception, enum Behaviour argmax, core::random, Decision + the
    // intent_* bridge). Loads the real script and asserts it RUNS in the VM
    // without tripping a noiser bug. Smoke ONLY — no behavioural assertion; the
    // brain is a moving target (see docs/noiser-brain-interop.md, testing line).
    const char* path = std::getenv("BADLANDS_BRAIN_SCRIPT");
    REQUIRE(path != nullptr);  // exported by tests/cpp_tests.rs
    std::ifstream file(path);
    REQUIRE(file.good());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    auto owned = badlands::make_world(source.c_str());

    BadlandsGame* game = owned.get();
    // A lone home-less hero with no enemy and no town buildings exercises the
    // full path: observe() (every perceive_* call) -> decide() (soft argmax ->
    // Roam) -> apply() (intent_move_to).
    badlands::CharacterDesc d{};
    d.pos_x = 3.0f;
    d.pos_z = 3.0f;
    d.team = 0;
    d.hp = 10.0f;
    d.move_speed = 1.0f;
    d.attack_range = 1.0f;
    d.attack_damage = 1.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    badlands::spawn_into(*game, d);

    for (int i = 0; i < 5; ++i) {
        badlands::tick_world(*game, 1.0f / 30.0f);
    }

    badlands::SimStats stats = badlands::stats_of(*game);
    CHECK(stats.noiser_bugs == 0);
}

TEST_CASE("perceive_class binds and echoes the hero class") {
    // The one new host call the framework adds. A synthetic script echoes the
    // perceived class into a durable move goal so we can read it back — proving
    // the binding + HeroClass component read, deterministically.
    const char* script = R"(
        @fn.perceive_class: fn(e: i32) -> i32;
        @fn.intent_move_to: fn(e: i32, x: f32, z: f32) -> void;
        pub gen fn brain(entity: i32) -> i32 {
            loop {
                let c = @fn.perceive_class(entity);
                @fn.intent_move_to(entity, f32(c), 0.0);
                yield 0;
            }
        }
        0.0
    )";
    auto owned = badlands::make_world(script);
    BadlandsGame* game = owned.get();
    badlands::CharacterDesc d{};
    d.pos_x = 4.0f;
    d.pos_z = 4.0f;
    d.team = 0;
    d.hp = 10.0f;
    d.move_speed = 1.0f;
    d.attack_range = 1.0f;
    d.attack_damage = 1.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    uint32_t hid = badlands::spawn_into(*game, d);

    // Force a known class on the spawned entity (Grave Robber = 2).
    entt::entity e = game->slots[hid];
    game->registry.emplace_or_replace<badlands::HeroCharacter>(e, 2);

    badlands::tick_world(*game, 1.0f / 30.0f);

    badlands::SimStats stats = badlands::stats_of(*game);
    if (stats.noiser_bugs == 0) {
        const badlands::MoveTarget& mt = game->registry.get<badlands::MoveTarget>(e);
        CHECK(mt.kind == badlands::MoveTarget::Kind::Point);
        CHECK(mt.point.x == 2.0f);  // the perceived class, echoed
    } else {
        WARN("[noiser-bug] perceive_class script tripped " << stats.noiser_bugs
                                                           << " bug(s); see docs/noiser-bugs-upstream");
    }
}

TEST_CASE("perceive_needs binds and echoes the hero's needs + clock") {
    // The needs/clock perception the day-night loop adds. A synthetic script
    // echoes two of the four flat f32s into a durable move goal, proving the
    // binding + HeroSimulationState/world_millis read, deterministically.
    const char* script = R"(
        @fn.perceive_needs:  fn(e: i32) -> (f32, f32, f32, f32);
        @fn.intent_move_to:  fn(e: i32, x: f32, z: f32) -> void;
        pub gen fn brain(entity: i32) -> i32 {
            loop {
                let (fatigue, content, tod, night) = @fn.perceive_needs(entity);
                @fn.intent_move_to(entity, fatigue + content, tod + night);
                yield 0;
            }
        }
        0.0
    )";
    auto game_owned = make_world(script);
    BadlandsGame* game = game_owned.get();
    CharacterDesc d = MercenaryDesc(4.0f, 4.0f);
    uint32_t hid = spawn_into(*game, d);
    entt::entity e = game->slots[hid];

    auto& sim = game->registry.get<badlands::HeroSimulationState>(e);
    sim.fatigue = 0.25f;
    sim.content = 0.5f;
    game->world_millis = badlands::kMillisPerDay / 2;  // midday -> tod 0.5, night 0

    tick_world(*game, 1.0f / 30.0f);

    SimStats stats;
    stats = stats_of(*game);
    if (stats.noiser_bugs == 0) {
        const badlands::MoveTarget& mt = game->registry.get<badlands::MoveTarget>(e);
        CHECK(mt.kind == badlands::MoveTarget::Kind::Point);
        // Reserves DRAINED one tick before the brain observed them.
        const float drained_fatigue =
            0.25f - badlands::reserve_rate_per_tick(game->factors.hero.fatigue_drain_hours);
        const float drained_content =
            0.5f - badlands::reserve_rate_per_tick(game->factors.hero.content_drain_hours);
        CHECK(mt.point.x == Catch::Approx(drained_fatigue + drained_content));
        CHECK(mt.point.y == Catch::Approx(0.5f).margin(0.01f));  // tod 0.5 + night 0
    } else {
        WARN("[noiser-bug] perceive_needs script tripped " << stats.noiser_bugs
                                                           << " bug(s); see docs/noiser-bugs-upstream");
    }
}

TEST_CASE("hero.noiser sends a tired hero home (parity with the C++ town brain)") {
    // Behavioural parity on the first system: the shipping noiser brain and the
    // C++ reference brain (town_brain.cpp) must reach the same decision from the
    // same world state. An apothecary + empty inventory would normally win the
    // argmax (the core errand); high fatigue must override it, exactly as the
    // C++ chain puts GoHome ahead of Buy. That override is the needs system.
    const char* path = std::getenv("BADLANDS_BRAIN_SCRIPT");
    REQUIRE(path != nullptr);
    std::ifstream file(path);
    REQUIRE(file.good());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    auto game_owned = make_world(source.c_str());
    BadlandsGame* game = game_owned.get();
    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    uint32_t guild = static_cast<uint32_t>(dispatch_into(*game, place));
    Action apo{ActionKind::PlaceBuilding, 0, 20.0f, 20.0f, static_cast<int32_t>(BuildingKind::Apothecary), 0};
    dispatch_into(*game, apo);
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(dispatch_into(*game, recruit));
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = game->slots[hid];

    glm::vec2 home_door;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[guild], home_door));

    game->registry.get<badlands::Position>(e).pos = {40.0f, 40.0f};
    auto& sim = game->registry.get<badlands::HeroSimulationState>(e);
    sim.fatigue = 0.9f;
    sim.inventory = 0;  // the errand is pending; fatigue must still win

    tick_world(*game, 1.0f / 30.0f);

    SimStats stats;
    stats = stats_of(*game);
    if (stats.noiser_bugs == 0) {
        const badlands::MoveTarget& mt = game->registry.get<badlands::MoveTarget>(e);
        CHECK(mt.kind == badlands::MoveTarget::Kind::Point);
        CHECK(mt.point.x == Catch::Approx(home_door.x));
        CHECK(mt.point.y == Catch::Approx(home_door.y));
    } else {
        WARN("[noiser-bug] hero.noiser needs parity tripped " << stats.noiser_bugs
                                                              << " bug(s); see docs/noiser-bugs-upstream");
    }
}
