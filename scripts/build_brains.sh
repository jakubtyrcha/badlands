#!/usr/bin/env bash
# Compiles the Nim "brain" sources (scripts/brains/nim/*.nim) to wasm32-wasi
# modules the brainhost crate (src/crates/brainhost) can load: for each brain,
# Nim's C backend generates C, which wasi-sdk's clang/wasm-ld turns into a
# freestanding wasm32 module.
#
# Builds three brains, all with the identical flag family (see build_one
# below): scripts/brains/nim/hero.nim -> assets/brains/hero.wasm (the
# shipping hero decision layer -- a port of game/src/town_brain.cpp +
# game/src/behaviours/{blocks,selectors,deliberation}.cpp), scripts/brains/
# nim/idle_test.nim -> game/tests/fixtures/idle_brain.wasm (a test-only
# fixture whose bl_tick always decides Idle -- what hero.wasm itself used to
# be, before Task 5's real decision port; existing tests that assert all-Idle
# behaviour target this instead), and scripts/brains/nim/trap_test.nim ->
# game/tests/fixtures/trap_brain.wasm (a test-only fixture whose bl_tick
# unconditionally traps -- game/tests/wasm_brain_tests.cpp's BhInstance
# reinstantiation coverage).
#
# HARD CONSTRAINT (enforced by brainhost's bh_instantiate, not by this
# script): the resulting module must import AT MOST env.bl_log -- no WASI
# imports. The flag set below is what currently achieves that; see the
# comment above the `nim c` invocation if you need to change it, and re-run
# `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` (the
# real_hero_wasm_conforms test) to check.
#
# Usage: run from anywhere; paths below are resolved relative to this
# script's location, not the caller's cwd.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# wasi-sdk: pinned release, downloaded once into third_party/toolchains/
# (gitignored) and cached there across runs.
# ---------------------------------------------------------------------------
WASI_SDK_RELEASE="wasi-sdk-33"     # the release tag on github.com/WebAssembly/wasi-sdk
WASI_SDK_VERSION="33.0"            # the version embedded in the tarball/dir name
WASI_SDK_ASSET="wasi-sdk-${WASI_SDK_VERSION}-arm64-macos.tar.gz"
WASI_SDK_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/${WASI_SDK_RELEASE}/${WASI_SDK_ASSET}"
WASI_SDK_SHA256="85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78"
WASI_SDK_DIRNAME="wasi-sdk-${WASI_SDK_VERSION}-arm64-macos"

TOOLCHAINS_DIR="$REPO_ROOT/third_party/toolchains"
WASI_SDK_DIR="$TOOLCHAINS_DIR/$WASI_SDK_DIRNAME"
CLANG="$WASI_SDK_DIR/bin/clang"

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "build_brains.sh: pinned wasi-sdk asset is arm64-macos only (got $(uname -s)/$(uname -m))" >&2
    exit 1
fi

if [[ -d "$WASI_SDK_DIR" ]]; then
    echo "build_brains.sh: wasi-sdk already present at $WASI_SDK_DIR (skipping download)"
else
    echo "build_brains.sh: downloading $WASI_SDK_ASSET ..."
    mkdir -p "$TOOLCHAINS_DIR"
    TMP_TARBALL="$(mktemp -t wasi-sdk-download).tar.gz"
    trap 'rm -f "$TMP_TARBALL"' EXIT
    curl -fL --progress-bar -o "$TMP_TARBALL" "$WASI_SDK_URL"

    ACTUAL_SHA256="$(shasum -a 256 "$TMP_TARBALL" | awk '{print $1}')"
    if [[ "$ACTUAL_SHA256" != "$WASI_SDK_SHA256" ]]; then
        echo "build_brains.sh: sha256 mismatch for $WASI_SDK_ASSET" >&2
        echo "  expected: $WASI_SDK_SHA256" >&2
        echo "  actual:   $ACTUAL_SHA256" >&2
        exit 1
    fi

    tar -xzf "$TMP_TARBALL" -C "$TOOLCHAINS_DIR"
    rm -f "$TMP_TARBALL"
    trap - EXIT
    echo "build_brains.sh: extracted to $WASI_SDK_DIR"
fi

if [[ ! -x "$CLANG" ]]; then
    echo "build_brains.sh: expected clang at $CLANG (wasi-sdk layout changed?)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Nim: install via brew if missing (approved -- see task-2-brief.md section B).
# ---------------------------------------------------------------------------
if ! command -v nim >/dev/null 2>&1; then
    echo "build_brains.sh: nim not found, installing via brew ..."
    brew install nim
fi
echo "build_brains.sh: $(nim --version | head -1)"

# ---------------------------------------------------------------------------
# Compile scripts/brains/nim/hero.nim -> assets/brains/hero.wasm.
#
# Flag notes (each one earns its place -- see the brief's "known-good flag
# family" starting point, adapted against the brainhost import validator):
#   --os:any --mm:arc -d:useMalloc --threads:off  the minimal freestanding
#       Nim runtime: no thread-local-storage setup, malloc-backed ARC (no GC
#       thread hooks).
#   -d:noSignalHandler   system.nim's default SIGINT/SIGSEGV handler needs
#       <signal.h>, which wasi-libc stubs out entirely (`#error`); this
#       define compiles that handler out instead of merely disabling it.
#   --panics:on --exceptions:quirky   the default "goto-based" exception
#       model emits an unconditional nimTestErrorFlag() check in every
#       module's init that calls exit(1) on an unhandled exception -- dead
#       code at runtime (this brain never raises) but the wasm-ld reference
#       to `exit` still drags in libc's exit -> _Exit -> the
#       wasi_snapshot_preview1.proc_exit IMPORT, which fails brainhost's
#       import validation regardless of reachability. Quirky exceptions drop
#       that check entirely; --panics:on additionally makes a Defect
#       (out-of-bounds, etc.) an unrecoverable trap rather than a raise.
#   --boundChecks:off --rangeChecks:off --overflowChecks:off   the SAME
#       "dead code still drags in a disallowed import" problem as the `exit`
#       one above, hit for real by Task 5's hero decision layer: even with
#       --panics:on turning a failed runtime check into a trap, the trap's
#       own message-formatting path (system.nim's fatal-error writer) still
#       references `fwrite` on a `File` handle -> the disallowed
#       wasi_snapshot_preview1.fd_write/fd_close/fd_seek/proc_exit IMPORTS --
#       never reached (every check this module could trip is either already
#       guarded or, for overflow, mathematically impossible at the sizes in
#       play), but still linked. Bisected by hand against the real hero.nim
#       build (not a guess): bound+range checks off ALONE still pulled the
#       four WASI imports; adding --overflowChecks:off on top of those two is
#       what actually cleared them (traced to rng.nim's range_i64, whose
#       `hi - lo` is signed int64 subtraction); --nilChecks:off/
#       --fieldChecks:off/--assertions:off, tried individually alongside
#       bound+range, did NOT move the needle, so they are deliberately absent
#       here -- this is the narrowest --checks:off subset that keeps the
#       artifact WASI-clean, not the blanket flag. (Assertion/nil/field
#       checks stay ON, for whatever that is worth in a --panics:on/
#       --exceptions:quirky build where a Defect traps either way.)
#   -mexec-model=reactor   selects wasi-libc's "reactor" C runtime (an
#       `_initialize` export, no `main`) over the default "command" one
#       (`_start` calling `main(argc, argv)`), which otherwise imports
#       wasi_snapshot_preview1.args_get/args_sizes_get to build argv this
#       module never uses.
#   -Wl,--no-entry -Wl,--allow-undefined -Wl,--export=bl_*   no wasm "start"
#       function, tolerate the one intentionally-undefined symbol (bl_log,
#       resolved by the host at instantiation), export exactly the ABI
#       surface brainhost requires.
# ---------------------------------------------------------------------------
# build_one <nim-src> <out-wasm> <nimcache-subdir>: identical flags for every
# brain, differing only in source/output paths -- so a new brain (like
# trap_test.nim below) is a one-line addition, not a copy of this whole block.
build_one() {
    local nim_src="$1" out_wasm="$2" nimcache_name="$3"
    local nimcache_dir="$TOOLCHAINS_DIR/nimcache/$nimcache_name"  # gitignored, never committed

    mkdir -p "$(dirname "$out_wasm")"
    rm -rf "$nimcache_dir"
    mkdir -p "$nimcache_dir"

    nim c \
        --cpu:wasm32 --os:any --mm:arc -d:useMalloc --threads:off -d:release --nomain \
        -d:noSignalHandler --panics:on --exceptions:quirky \
        --boundChecks:off --rangeChecks:off --overflowChecks:off \
        --cc:clang --clang.exe:"$CLANG" --clang.linkerexe:"$CLANG" \
        --passC:"--target=wasm32-wasip1" \
        --passL:"--target=wasm32-wasip1 -mexec-model=reactor -Wl,--no-entry -Wl,--allow-undefined -Wl,--export=bl_abi_version -Wl,--export=bl_init -Wl,--export=bl_spawn -Wl,--export=bl_despawn -Wl,--export=bl_view_buf -Wl,--export=bl_out_buf -Wl,--export=bl_tick" \
        --nimcache:"$nimcache_dir" \
        -o:"$out_wasm" \
        "$nim_src"

    echo "build_brains.sh: wrote $out_wasm ($(wc -c < "$out_wasm") bytes)"
}

build_one "$SCRIPT_DIR/brains/nim/hero.nim" "$REPO_ROOT/assets/brains/hero.wasm" hero
build_one "$SCRIPT_DIR/brains/nim/idle_test.nim" \
    "$REPO_ROOT/game/tests/fixtures/idle_brain.wasm" idle_test
build_one "$SCRIPT_DIR/brains/nim/trap_test.nim" \
    "$REPO_ROOT/game/tests/fixtures/trap_brain.wasm" trap_test
