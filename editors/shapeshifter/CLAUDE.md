# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working under
`editors/shapeshifter/`. The repo-wide rules in the root `CLAUDE.md` still
apply; this file covers what is different here.

## What this is

Shapeshifter — a native macOS 3D editor built on SDF rendering with direct, intuitive editing. UI/UX design is provided by the user; don't invent design.

## Architecture

Two layers, joined by direct Swift↔C++ interop (Swift C++ interop mode, no bridge files). Keep the interop surface small and data-oriented; avoid heavy templates or move-only types at the boundary.

- **Swift app shell** — SwiftUI for app chrome (windows, panels, inspectors). The editor viewport is a CAMetalLayer-backed NSView (AppKit) that owns all raw input — touchpad gestures, pressure, phases via NSEvent; Apple Pencil planned longer-term — and forwards it to the core.
- **C++ rendering core** — renders through the engine's RHI and render graph (`src/engine/rhi`, `src/engine/graph`), never Metal directly. Owns the device, the seven pipelines, the frame allocator and the swapchain; shaders are Slang, compiled at startup. Complex editor UI may also be rendered here.

Coordinates: right-handed, Y up (same convention as `../raycast`). Metal clip-space z is [0,1] — pick projection math accordingly.

## Build — two systems, one seam

The split follows the layer boundary above, and is deliberate: CMake is a poor
fit for a SwiftUI `.app` bundle, and Xcode is a poor fit for the rest of this
repo, so each keeps what it is good at.

| Half | Built by | Targets |
|---|---|---|
| C++ core + its tests | the repo-root `CMakeLists.txt` | `shapeshifter_core`, `shapeshifter_core_tests`, `shapeshifter_rhi_tests` |
| Slang shaders | compiled at startup by `badlands_slang`; `ctest -R shapeshifter_slang` compiles them in CI | — |
| Swift app bundle | XcodeGen, `project.yml` | `Shapeshifter` |

- `scripts/build.sh shapeshifter_core` — build the core (from the repo root).
- `scripts/test.sh shapeshifter_core_tests` — run its suite under ctest.
- `xcodegen generate` — regenerate `.xcodeproj` after editing `project.yml`;
  never hand-edit the generated project.
- `xcodebuild -scheme Shapeshifter build` — build the app. Its pre-build phase
  runs `scripts/build.sh shapeshifter_core` first and then links the resulting
  `build/libshapeshifter_core.a`, so this one command builds both halves.

Two test suites, split along a real seam: `shapeshifter_core_tests` is doctest
and covers the pure-CPU core (scene, picking, gizmo math); `shapeshifter_rhi_tests`
is Catch2 and covers what the editor does to a device, because it links the RHI
and the graph whose own suites are Catch2. The doctest half is a leftover from
the standalone repo, but the split is now load-bearing rather than accidental.

## Shaders are STAGED, hashed, and bundled — never read from the source tree

The editor compiles Slang at startup, so where it reads from is a correctness
question, not a packaging one. A binary that reads shaders newer than itself
fails every pipeline and shows a black window, with the reason on a stderr that
a Finder launch throws away. That happened. So:

- **CMake stages one canonical tree** (`cmake/ShapeshifterShaders.cmake`) into
  `build/shaders_staged/`: the editor's shaders, the engine's `slang/common`
  modules, and a `MANIFEST` holding the tree's content hash. The same hash is
  baked into the binary as a generated header.
- **Two tiers, in order, never merged** (`core/src/shader_paths.h`): the app
  bundle's `Contents/Resources/shaders` if its `MANIFEST` exists, else the
  staged tree. A tier whose manifest disagrees with the binary is REFUSED, with
  the path and both hashes, on stderr *and* `os_log`.
- **Tier 1 is chosen by MANIFEST presence, not by "am I bundled".**
  `CFBundleCopyResourcesDirectoryURL` returns the executable's own directory for
  a non-bundled binary, so a bare binary beside a `shaders/MANIFEST` would
  select it — which is why staging goes to `shaders_staged` and never `shaders`.
- **A shader-only edit rebuilds and re-bundles.** The baked hash lives in a
  generated header, so editing a `.slang` relinks the core; the Xcode copy phase
  runs unconditionally. There is no hot reload — rebuild.
- **Never add a fourth search-path list.** `editor.cpp`, the test compiler helper
  and the `slangc` tests all derive from `SHAPESHIFTER_STAGED_INCLUDES`; three
  hand-maintained copies had already drifted once.
- **`shapeshifter_presented_frame` is the gate that matters.** It renders through
  a real `CAMetalLayer` drawable and reads the pixels back, because every suite
  was green both times the window was black. It is behind `ctest -L display`;
  the always-on cover is the headless dump, which touches no layer.

## The frame is three passes

`geometry` (colour clear + depth clear/write: raymarch, mesh) → `ground`
(colour load + depth read-only: the plate, the origin marker) → `chrome`
(colour load, **no depth attachment**: scene lines, gizmo, focus dot, pivot).

- **The chrome pass has no depth attachment, and its pipelines declare no depth
  format.** Metal validation requires the two to agree; "both say none"
  satisfies it as surely as "both say Depth32Float".
- **Seven pipelines, where the metal-cpp path had five PSOs.** The RHI folds
  depth state and primitive topology into the pipeline, so what Metal let a
  draw choose is now chosen where the pipeline is built.
- **Two blend states, and neither is `rhi::AlphaBlend()`.** The ground plate is
  premultiplied (`ground_grid_shade` returns coverage already multiplied
  through); everything else is straight alpha.
- **Uniforms and vertices share one frame allocator.** A binding table is
  immutable, so it can only follow the frame if the buffer stays put and the
  offsets move — which is why both usages ride one ring.
- **Offsets go to `SetBindingTable` in increasing slot order.** Reversed, each
  lands on the wrong binding and the trace reads uniform bytes as nodes.
- **The frame is paced by a plain `CADisplayLink`, and NOT `CAMetalDisplayLink`.**
  The swapchain calls `nextDrawable` itself, so a display link that also vends
  one puts two consumers on a pool of two or three — it empties, and the link
  silently stops calling back.
- **`CADisplayLink` retains its target strongly**, where `CAMetalDisplayLink`'s
  delegate was weak. `MetalViewport.dismantleNSView` invalidating the driver is
  what keeps that from leaking the editor and ticking into a dead layer.
