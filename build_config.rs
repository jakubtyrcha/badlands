// Single source of truth for the C++ build inputs, shared by build.rs (the
// app static lib) and tests/cpp_tests.rs (the Catch2 binary) via include!.
// Paths are relative to the repo root. Adding a source or include dir here
// updates the app build, the test build, and the test-harness staleness
// check together.

pub const CXX_STD: &str = "c++23";

pub const EXTRA_CXX_FLAGS: &[&str] = &[
    // noiser_common.h intentionally ships [[deprecated]] shims.
    "-Wno-deprecated-declarations",
];

pub const GAME_SOURCES: &[&str] = &[
    "game/src/game.cpp",
    "game/src/brain.cpp",
    "game/src/placement.cpp",
    "game/src/noiser_jit_stubs.cpp",
];

// The sampo::noiser wrapper TUs (pure delegation over the Rust VM's C ABI).
pub const WRAPPER_SOURCES: &[&str] = &[
    "third_party/noiser/noiser-vm/src/noiser.cpp",
    "third_party/noiser/noiser-vm/src/noiser_crash.cpp",
];

pub const TEST_SOURCES: &[&str] = &[
    "game/tests/sim_tests.cpp",
    "game/tests/noiser_smoke_tests.cpp",
    "game/tests/duel_test.cpp",
    "game/tests/placement_tests.cpp",
    "third_party/catch2/extras/catch_amalgamated.cpp",
];

pub const INCLUDE_DIRS: &[&str] = &[
    "game/include",
    "third_party/noiser/noiser-vm/src",
    "third_party/noiser",
    "third_party/glm",
    "third_party/spdlog/include",
    "third_party/entt/single_include",
];

// Tests reach into the sim's internal headers (game_state.h, placement.h) for
// white-box checks, so game/src joins the Catch2-only include path.
pub const TEST_INCLUDE_DIRS: &[&str] = &["third_party/catch2/extras", "game/src"];
