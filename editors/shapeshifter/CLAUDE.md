# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working under
`editors/shapeshifter/`. The repo-wide rules in the root `CLAUDE.md` still
apply; this file covers what is different here.

## What this is

Shapeshifter — a native macOS 3D editor built on SDF rendering with direct, intuitive editing. UI/UX design is provided by the user; don't invent design.

## Architecture

Two layers, joined by direct Swift↔C++ interop (Swift C++ interop mode, no bridge files). Keep the interop surface small and data-oriented; avoid heavy templates or move-only types at the boundary.

- **Swift app shell** — SwiftUI for app chrome (windows, panels, inspectors). The editor viewport is a CAMetalLayer-backed NSView (AppKit) that owns all raw input — touchpad gestures, pressure, phases via NSEvent; Apple Pencil planned longer-term — and forwards it to the core.
- **C++ rendering core** — owns Metal via metal-cpp: device, queues, pipelines, SDF evaluation/rendering, drawable pacing (CAMetalDisplayLink). Complex editor UI may also be Metal-rendered here.

Coordinates: right-handed, Y up (same convention as `../raycast`). Metal clip-space z is [0,1] — pick projection math accordingly.

## Build — two systems, one seam

The split follows the layer boundary above, and is deliberate: CMake is a poor
fit for a SwiftUI `.app` bundle, and Xcode is a poor fit for the rest of this
repo, so each keeps what it is good at.

| Half | Built by | Targets |
|---|---|---|
| C++ core + its tests | the repo-root `CMakeLists.txt` | `shapeshifter_core`, `shapeshifter_core_tests` |
| Swift app + `.metal` shaders | XcodeGen, `project.yml` | `Shapeshifter` |

- `scripts/build.sh shapeshifter_core` — build the core (from the repo root).
- `scripts/test.sh shapeshifter_core_tests` — run its suite under ctest.
- `xcodegen generate` — regenerate `.xcodeproj` after editing `project.yml`;
  never hand-edit the generated project.
- `xcodebuild -scheme Shapeshifter build` — build the app. Its pre-build phase
  runs `scripts/build.sh shapeshifter_core` first and then links the resulting
  `build/libshapeshifter_core.a`, so this one command builds both halves.

The core suite is still on doctest rather than the repo's Catch2. That is a
known inconsistency from the move, not a decision — port it when touching the
tests anyway.

## Not yet on the RHI

The core still drives Metal directly through vendored metal-cpp
(`vendor/metal-cpp`), not through `src/engine/rhi`. Porting it is the next step
and the reason this lives here at all, so prefer changes that shrink the
metal-cpp surface over ones that widen it. The two seams that matter are
already compatible: `SwapchainDesc::native_window` takes the `CAMetalLayer*`
the viewport already owns, and `IShaderModule` takes MSL source — though it
also requires a `ShaderReflection`, which is what will pull the shaders onto
Slang.
