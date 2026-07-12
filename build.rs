// Builds the C++ game (EnTT sim + sampo::noiser wrapper) into a static lib.
// The wrapper's noiser_vm_*/noiser_compile* references resolve at the final
// link against the noiser-vm/noiser-compiler rlibs' #[no_mangle] exports.
// Sources/includes/flags live in build_config.rs, shared with the C++ test
// harness (tests/cpp_tests.rs).

#[allow(dead_code)]
mod config {
    include!("build_config.rs");
}
use config::*;

fn main() {
    let mut build = cc::Build::new();
    build.cpp(true).std(CXX_STD);
    for dir in INCLUDE_DIRS {
        build.include(dir);
    }
    for flag in EXTRA_CXX_FLAGS {
        build.flag_if_supported(flag);
    }
    for file in GAME_SOURCES.iter().chain(WRAPPER_SOURCES) {
        build.file(file);
    }
    build.compile("badlands_game");

    // The C++ test harness reuses cc's compiler discovery and needs the
    // build triple at test-compile time.
    println!("cargo:rustc-env=BADLANDS_TARGET={}", std::env::var("TARGET").unwrap());
    println!("cargo:rustc-env=BADLANDS_HOST={}", std::env::var("HOST").unwrap());

    // game/tests is test-only: editing it must not rebuild the app lib.
    println!("cargo:rerun-if-changed=game/src");
    println!("cargo:rerun-if-changed=game/include");
    println!("cargo:rerun-if-changed=third_party/noiser/noiser-vm/src");
    println!("cargo:rerun-if-changed=build_config.rs");
}
