//! Builds and runs the C++ Catch2 test suite (game/tests/) so that plain
//! `cargo test` covers the whole stack.
//!
//! The C++ test binary cannot link Rust rlibs, so it links the aggregated
//! `noiser-bundle` staticlib (all noiser FFI symbols, libstd exactly once).
//! Sources/includes/flags come from build_config.rs — the same lists
//! build.rs compiles the app from — and the rebuild-staleness check derives
//! from those lists, so it cannot drift from the compile line.

#[allow(dead_code)]
mod config {
    include!("../build_config.rs");
}
use config::*;

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

fn all_sources() -> impl Iterator<Item = &'static str> {
    GAME_SOURCES
        .iter()
        .chain(WRAPPER_SOURCES)
        .chain(TEST_SOURCES)
        .copied()
}

// Everything the rebuild-staleness decision looks at: every compile input
// (sources and include trees), the Rust bundle, this harness, and the shared
// build config.
fn staleness_roots(root: &Path) -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = all_sources().map(|s| root.join(s)).collect();
    roots.extend(
        INCLUDE_DIRS
            .iter()
            .chain(TEST_INCLUDE_DIRS)
            .map(|d| root.join(d)),
    );
    roots.push(root.join("target/debug/libnoiser_bundle.a"));
    roots.push(root.join("tests/cpp_tests.rs"));
    roots.push(root.join("build_config.rs"));
    roots
}

#[test]
fn staleness_check_covers_every_compile_input() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let roots = staleness_roots(&root);
    for source in all_sources() {
        let path = root.join(source);
        assert!(
            roots.iter().any(|r| path.starts_with(r)),
            "compile input {source} is invisible to the rebuild check"
        );
    }
}

// Newest mtime of a file, or of any file under a directory.
fn newest_mtime(path: &Path) -> SystemTime {
    let mut newest = std::fs::metadata(path)
        .and_then(|m| m.modified())
        .unwrap_or(SystemTime::UNIX_EPOCH);
    let mut stack = vec![path.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if let Ok(modified) = entry.metadata().and_then(|m| m.modified()) {
                newest = newest.max(modified);
            }
        }
    }
    newest
}

fn run(command: &mut Command) {
    let rendered = format!("{command:?}");
    let status = command.status().unwrap_or_else(|e| panic!("failed to run {rendered}: {e}"));
    assert!(status.success(), "command failed ({status}): {rendered}");
}

#[test]
fn cpp_game_tests() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));

    // 1. The aggregated noiser staticlib for the C++ side.
    run(Command::new(env!("CARGO"))
        .args(["build", "-p", "noiser-bundle"])
        .current_dir(&root));
    let bundle = root.join("target/debug/libnoiser_bundle.a");
    assert!(bundle.exists(), "missing {}", bundle.display());

    // 2. The Catch2 binary, rebuilt only when inputs changed.
    let out_dir = root.join("target/cpp-tests");
    std::fs::create_dir_all(&out_dir).unwrap();
    let binary = out_dir.join("badlands_game_tests");
    let binary_time = std::fs::metadata(&binary)
        .and_then(|m| m.modified())
        .unwrap_or(SystemTime::UNIX_EPOCH);

    if staleness_roots(&root).iter().any(|p| newest_mtime(p) > binary_time) {
        // Same compiler discovery as build.rs (honors CXX etc.); the flags
        // are appended explicitly in case the discovered Tool omits them.
        let compiler = cc::Build::new()
            .cpp(true)
            .cargo_metadata(false)
            .target(env!("BADLANDS_TARGET"))
            .host(env!("BADLANDS_HOST"))
            .opt_level(1)
            .debug(true)
            .std(CXX_STD)
            .get_compiler();
        let mut compile = compiler.to_command();
        compile.arg(format!("-std={CXX_STD}"));
        compile.args(["-g", "-O1"]);
        compile.args(EXTRA_CXX_FLAGS);
        for dir in INCLUDE_DIRS.iter().chain(TEST_INCLUDE_DIRS) {
            compile.arg("-I").arg(root.join(dir));
        }
        for source in all_sources() {
            compile.arg(root.join(source));
        }
        compile.arg(&bundle);
        compile.arg("-o").arg(&binary);
        compile.current_dir(&root);
        run(&mut compile);
    }

    // 3. Run it; Catch2 output goes straight to the terminal.
    run(Command::new(&binary)
        .env(
            "BADLANDS_BRAIN_SCRIPT",
            root.join("scripts/brains/warrior.noiser"),
        )
        .current_dir(&root));
}
