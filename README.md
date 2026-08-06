# Badlands

A Majesty-style town-and-heroes prototype: a C++/Dawn (WebGPU)/SDL3 engine driving a
C++/EnTT simulation, with per-entity AI "brains". The hero brain runs as Nim
compiled to WASM (hosted via wasmtime), the sole hero decision layer.

## Getting started

This repo uses **git submodules** (third-party C++ libraries under
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
`*.ozz`, `*.png`, `*.ttf`, and `*.wasm` — the fonts under `assets/fonts/`, the PBR material
packs under `assets/materials/`, the brain modules under `assets/brains/`, and the
skeleton + animation clips under `assets/characters/`. New binaries of those types are tracked automatically; a
plain `git add` on them stores an LFS pointer, not the blob. Keep large binaries in
one of those extensions (or add the extension to `.gitattributes`) so the repo stays
lean.

## Build & run

Run from the repo root (`shaders/` + `assets/` resolve relative to cwd).

**CMake 3.30 or newer is required** — that floor comes from `third_party/ozz-animation`
(the skeletal-animation runtime), not from our own build files.

```sh
cmake -S . -B build -G Ninja    # configure (first Dawn-from-source build is long, then cached)
cmake --build build              # build the apps + Rust staticlibs
./build/badlands_game            # run (opens an SDL3 window)
ctest --test-dir build           # C++ Catch2 test suites
```

The C++ suite covers the hero brain (`scripts/brains/nim/hero.nim`, built to
`assets/brains/hero.wasm`); see `docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md`
for the wasm brain contract.

## Shapeshifter — the SDF editor

`editors/shapeshifter/` is the SDF editor for this project, moved in from its own
repo. Its C++ core is a normal CMake target here (`shapeshifter_core`, tested by
`shapeshifter_core_tests`); its SwiftUI app shell stays on XcodeGen, because CMake
is a poor fit for a `.app` bundle. Building the app builds both halves:

```sh
cd editors/shapeshifter
xcodegen generate                                                   # needs `brew install xcodegen`
xcodebuild -scheme Shapeshifter build -derivedDataPath DerivedData
```

See `editors/shapeshifter/README.md`.
