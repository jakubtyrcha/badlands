#!/usr/bin/env bash
# Compiles the Nim "brain" sources (scripts/brains/nim/*.nim) to wasm32-wasi
# modules the brainhost crate (src/crates/brainhost) can load: for each brain,
# Nim's C backend generates C, which wasi-sdk's clang/wasm-ld turns into a
# freestanding wasm32 module.
#
# Today this builds exactly one brain: scripts/brains/nim/hero.nim ->
# assets/brains/hero.wasm.
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
NIM_SRC="$SCRIPT_DIR/brains/nim/hero.nim"
OUT_DIR="$REPO_ROOT/assets/brains"
OUT_WASM="$OUT_DIR/hero.wasm"
NIMCACHE_DIR="$TOOLCHAINS_DIR/nimcache/hero"  # under the gitignored toolchains dir, never committed

mkdir -p "$OUT_DIR"
rm -rf "$NIMCACHE_DIR"
mkdir -p "$NIMCACHE_DIR"

nim c \
    --cpu:wasm32 --os:any --mm:arc -d:useMalloc --threads:off -d:release --nomain \
    -d:noSignalHandler --panics:on --exceptions:quirky \
    --cc:clang --clang.exe:"$CLANG" --clang.linkerexe:"$CLANG" \
    --passC:"--target=wasm32-wasip1" \
    --passL:"--target=wasm32-wasip1 -mexec-model=reactor -Wl,--no-entry -Wl,--allow-undefined -Wl,--export=bl_abi_version -Wl,--export=bl_init -Wl,--export=bl_spawn -Wl,--export=bl_despawn -Wl,--export=bl_view_buf -Wl,--export=bl_out_buf -Wl,--export=bl_tick" \
    --nimcache:"$NIMCACHE_DIR" \
    -o:"$OUT_WASM" \
    "$NIM_SRC"

echo "build_brains.sh: wrote $OUT_WASM ($(wc -c < "$OUT_WASM") bytes)"
