# Badlands

A Majesty-style town-and-heroes prototype: a C++/Dawn (WebGPU)/SDL3 engine driving a
C++/EnTT simulation, with per-entity AI "brains". The hero brain runs as Nim
compiled to WASM (hosted via wasmtime); the [noiser](third_party/noiser) scripting
language still drives mapgen/texgen and remains available as a dormant brain
backend.

## Getting started

This repo uses **git submodules** (the `noiser` language toolchain under
`third_party/`) and **git LFS** (binary assets — fonts and PBR material packs). You
need `git-lfs` installed first (`brew install git-lfs`), then clone with both:

```sh
git lfs install                        # one-time, per machine
git clone --recurse-submodules <url>   # pulls submodules + LFS objects
cd badlands
```

Already cloned without them?

```sh
git submodule update --init --recursive
git lfs pull                           # fetch the ~35 MB of LFS binaries
```

## Git LFS

Binary assets live in git LFS. `.gitattributes` tracks `*.bin`, `*.jpg`, `*.jpeg`,
`*.png`, and `*.ttf` — the fonts under `assets/fonts/` and the PBR material packs
under `assets/materials/`. New binaries of those types are tracked automatically; a
plain `git add` on them stores an LFS pointer, not the blob. Keep large binaries in
one of those extensions (or add the extension to `.gitattributes`) so the repo stays
lean.

## Build & run

Run from the repo root (`shaders/` + `assets/` resolve relative to cwd).

```sh
cmake -S . -B build -G Ninja    # configure (first Dawn-from-source build is long, then cached)
cmake --build build              # build the apps + Rust staticlibs
./build/badlands_game            # run (opens an SDL3 window)
ctest --test-dir build           # C++ Catch2 test suites
```

The C++ suite covers the hero brain (`scripts/brains/nim/hero.nim`, built to
`assets/brains/hero.wasm`) and the duel's combat brain (`scripts/brains/combat_test.noiser`,
still noiser); see `docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md` for the
wasm brain contract, `docs/noiser-brain-interop.md` for the (dormant) noiser brain framework,
and `docs/noiser-bugs-upstream/` for filed language bugs.
