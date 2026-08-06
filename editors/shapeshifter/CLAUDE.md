# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Shapeshifter — a native macOS 3D editor built on SDF rendering with direct, intuitive editing. UI/UX design is provided by the user; don't invent design.

## Architecture

Two layers, joined by direct Swift↔C++ interop (Swift C++ interop mode, no bridge files). Keep the interop surface small and data-oriented; avoid heavy templates or move-only types at the boundary.

- **Swift app shell** — SwiftUI for app chrome (windows, panels, inspectors). The editor viewport is a CAMetalLayer-backed NSView (AppKit) that owns all raw input — touchpad gestures, pressure, phases via NSEvent; Apple Pencil planned longer-term — and forwards it to the core.
- **C++ rendering core** — owns Metal via metal-cpp: device, queues, pipelines, SDF evaluation/rendering, drawable pacing (CAMetalDisplayLink). Complex editor UI may also be Metal-rendered here.

Targets: `ShapeshifterCore` (C++ static lib) and `Shapeshifter` (Swift app).

Coordinates: right-handed, Y up (same convention as `../raycast`). Metal clip-space z is [0,1] — pick projection math accordingly.

## Build

Not yet scaffolded. Intended setup: XcodeGen — `project.yml` is the source of truth; never hand-edit the generated `.xcodeproj`.

- `xcodegen generate` — regenerate the project after editing `project.yml`
- `xcodebuild -scheme Shapeshifter build` — build
