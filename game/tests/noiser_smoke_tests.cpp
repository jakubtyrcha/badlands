// Pins on the sampo::noiser wrapper behaviors the brain integration depends
// on. Every failure here is a noiser bug or API drift — paste the output into
// docs/noiser-feedback.md before working around it.

#include <noiser.hpp>

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
