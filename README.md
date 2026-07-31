# Shapeshifter

Shapeshifter is a native macOS 3D SDF editor MVP: a SwiftUI + AppKit shell
over a C++20 metal-cpp core, joined by direct Swift↔C++ interop (no bridge
files). The viewport is a per-pixel sphere-traced render of the CSG scene,
evaluated every frame (see Rendering below); the full spawn/select/
modify/camera interaction model, including delete, is in place.

## Requirements

- macOS 15+
- Xcode (current)
- [XcodeGen](https://github.com/yonaskolb/XcodeGen): `brew install xcodegen`

## Build & run

```sh
xcodegen generate                                             # regenerate Shapeshifter.xcodeproj from project.yml
xcodebuild -scheme Shapeshifter build -derivedDataPath build  # build the app
open build/Build/Products/Debug/Shapeshifter.app
```

(Or open `Shapeshifter.xcodeproj` in Xcode and run the `Shapeshifter` scheme
directly.)

`project.yml` is the source of truth for the Xcode project — never hand-edit
the generated `.xcodeproj`; re-run `xcodegen generate` after changing it.

To build and run the core test suite:

```sh
xcodebuild -scheme CoreTests build -derivedDataPath build
./build/Build/Products/Debug/CoreTests
```

## Using the editor

Four modes, selected via the icon bar top-left or keys **1–4**:

1. **Select (1)** — click a shape to inspect it; the name appears in the
   top-right info panel. Click empty space to deselect.
2. **Spawn (2)** — a second row of options appears below the mode bar to
   choose the shape (cube/sphere) and operation (additive/subtract) for the
   next spawn. Click in the viewport to place it: clicking on an existing
   shape snaps the new one onto that surface; a miss lands it a fixed
   distance ahead of the camera along the click ray. Either way the new node
   is selected and the editor auto-switches to Modify mode.
3. **Modify (3)** — drag the selected shape to move it, constrained to its
   tangent plane (or a camera-facing plane if unsnapped). Entering Modify
   with nothing selected behaves like Select mode until you click something.
   A radial menu anchored on the selected shape offers: Move, Scale (drag
   vertically; cumulative, clamped per-axis to [0.05, 50]), toggle
   additive/subtract, and Delete (removes the shape permanently — no undo —
   and switches to Camera mode).
4. **Camera (4)** — two-finger scroll orbits, pinch zooms, shift+scroll pans.

Color legend: green = additive shape, red = subtracted shape, pale blue =
currently selected.

## Rendering

The viewport is a per-pixel sphere-traced render of the CSG scene
(`shaders/raymarch.metal` + `shaders/sdf_scene.h`), evaluated every frame —
edits and drags are visible live, with no reconstruction latency. Shading is
normal-colored debug (color = 0.5·(n+1), gradient normal — no lighting). The
wireframe draws for the selected object only (no selection, no wireframe).
The scene is capped at `kMaxRaymarchNodes` (128) shapes for the per-frame GPU
upload. The depth buffer is real: the raymarch fragment shader writes true
per-pixel depth, so later passes composite correctly on top of it.

### DCSDD

Dual Contouring of Signed Distance Data (Carrera et al., SIGGRAPH 2026) mesh
reconstruction is implemented and tested but dormant — not triggered, not
drawn, kept in-tree for a later iteration. See `core/src/dcsdd.h` for the
pipeline and its tunables.

## Architecture

Three targets: `ShapeshifterCore` (C++ static library), `Shapeshifter` (the
Swift app), and `CoreTests` (a doctest-based command-line test runner for
the core).

- **Core** (`core/src`) owns the scene document, orbit camera and its
  controller, ray-based picking, drag/scale math, DCSDD mesh reconstruction
  (dormant, background thread), and all Metal rendering (raymarch + wireframe
  + shaded-mesh pipelines, depth buffer, per-frame encode) via metal-cpp.
- **Swift** (`app/Sources`) owns the 4-mode state machine, raw AppKit input
  (mouse/keyboard/gesture events on the `CAMetalLayer`-backed viewport), and
  the SwiftUI chrome/overlays (mode bar, spawn options, info panel, radial
  menu). It talks to core only through `Editor`, the single C++↔Swift
  interop surface.

See `CLAUDE.md` for interop and coding conventions.

## Known limitations

- CSG min/max combine is not a true distance field near intersection curves
  (affects raymarch step sizes there; sphere tracing tolerates the resulting
  underestimates by design).
- No undo (delete is permanent).
- No rotations.
- No materials or lighting — shading is normal-colored debug only.
- No export/saving.
- Single window.
- Camera mode is trackpad-oriented: a mouse wheel only supplies vertical
  scroll deltas (no horizontal component), and a mouse has no pinch gesture,
  so zoom is unavailable.
