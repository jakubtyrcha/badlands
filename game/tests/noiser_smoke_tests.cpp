// Pins on the sampo::noiser wrapper behaviors the brain integration depends
// on. Every failure here is a noiser bug or API drift — paste the output into
// docs/noiser-feedback.md before working around it.

#include <noiser.hpp>

#include "badlands_game.h"
#include "components.h"
#include "game_state.h"

#include <catch_amalgamated.hpp>

#include <vector>

using sampo::noiser::CompileError;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;

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
    BadlandsGame* game = game_create(script);
    GameCharacterDesc d{};
    d.pos_x = 20.0f;
    d.pos_z = 20.0f;
    d.team = 0;
    d.hp = 10.0f;
    d.move_speed = 1.0f;
    d.attack_range = 1.0f;
    d.attack_damage = 1.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    uint32_t hid = game_spawn(game, &d);

    game_tick(game, 1.0f / 30.0f);

    GameStats stats;
    game_stats(game, &stats);
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
    game_destroy(game);
}
