# Shapeshifter

Shapeshifter is a native macOS 3D SDF editor MVP: a SwiftUI + AppKit shell
over a C++20 metal-cpp core, joined by direct Swift↔C++ interop (no bridge
files). The viewport is a per-pixel sphere-traced render of the CSG scene,
evaluated every frame (see Rendering below), navigated by an always-on
cursor-anchored camera.

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

**The camera is always available.** There is no camera mode: a drag that
doesn't grab a gizmo handle drives the camera, in every mode. A press only
becomes a drag once it travels ~4pt, so clicking still means click.

| Gesture | Does |
|---|---|
| drag from empty space | **orbit**, around whatever you pressed on |
| `⌥` drag | **pan**, on world axes |
| `⌘` drag (vertical) | **dolly**, toward the cursor |
| two-finger scroll | pan |
| pinch | dolly |
| drag from a gizmo handle | move / rotate / scale |
| click (no drag) | select, or spawn |
| `F` | frame the selection |

Orbit re-centres on the surface point under the press, so pointing at a
feature and dragging spins around *that feature*. Doing so never moves the
camera — it only changes what a rotation means — and a quiet dot tracks the
surface as you hover, showing where the pivot would land before you commit.
Pan runs on world axes: drag up and you travel up, whatever the camera angle.
Dolly converges on the point under the cursor rather than the screen centre.

Two modes, selected by the icons top-left or keys **1**–**2**:

1. **Edit (1)** — click a shape to select it; the name appears in the
   top-right info panel, and a radial menu anchored on the shape offers the
   additive/subtract toggle and Delete (permanent — no undo). Clicking empty
   space deselects. There is no tool to arm: a selected shape shows **both**
   of its manipulators at once, and the handle you grab decides what happens.
2. **Spawn (2)** — a second row of options chooses the shape and operation
   (additive/subtract). Click in the viewport to place it: clicking an existing
   shape snaps the new one onto that surface; a miss lands it a fixed distance
   ahead of the camera. Either way the new node is selected and the editor
   returns to Edit.

### The eight shapes, and the dial

| Shape | Dial | |
|---|---|---|
| Cone, Pyramid | tip ratio | 0 = point, 1 = cylinder / box |
| Cube, Octahedron | roundness | 0 = sharp, 1 = ball |
| Capsule | cap roundness | 0 = flat cylinder, 1 = capsule |
| Vesica | tip roundness | 0 = cusps, 1 = capsule |
| Prism | side count | 3 to 12 |
| Sphere | — | already the roundest thing its box allows |

A shape's **size** is its box — position, rotation and the three scale handles.
What a box cannot say is a shape's *profile*: how blunt a cone's tip is, how
round a capsule's caps are, how many faces a prism has. That one leftover
degree of freedom gets a dial. Every shape has one except the sphere, and
roundness is the same idea on four of them.

The dial is the **third seat in the radial menu**, between the op toggle and
Delete — it is the same kind of thing as those, something about the shape that
no gizmo handle can reach. At rest it shows its value: a ring gauge around the
rim with the number inside. Press and hold, and it becomes the thing you turn.
The arc lays itself out so the value you started on sits under your cursor, so
there is no jump on grab, and both ends are within half a turn of wherever you
grabbed. The track, its detents and the readout appear only while you hold it.
Continuous dials step in 0.05; the prism's side count steps in whole numbers.

### The two manipulators

A selected shape carries two gizmos, and which one you get is decided by which
handle you grab — nothing is armed, and nothing is a mode.

| Gizmo | Sits at | Handles | Axes follow |
|---|---|---|---|
| **Placement** | the shape's attachment point, or its centre if it has none | 3 move axes, 1 plane patch, 3 rotation rings | the surface it was placed on, or the shape's own axes |
| **Shape** | the shape's centre, always | 3 scale boxes, 1 centre box for uniform scale | the shape's own axes |

Their handles sit in disjoint rings of radius — uniform scale at the centre,
move axes inside, scale boxes outside, rotation rings outermost — so when the
two anchors coincide the pair simply reads as one gizmo. That is the usual
case: a shape spawned onto a surface is centred on the point you clicked.

The plane patch drags in two axes at once. There is one, not one per axis pair,
and it always lies in the same plane as the reference grid: the tangent plane
for an attached shape — the "slide the nose across the face" gesture — and
world-horizontal for a free one. The grid itself appears only while you are
dragging the Placement gizmo, which is the only time it answers anything.

Pull it off that surface and the pair separates, joined by a thin tether: the
Placement gizmo stays on the skin where the shape is attached, the Shape gizmo
rides the shape. Rotating turns the shape about the Placement anchor, so a
detail attached to a surface swings around its contact point instead of
drifting off it. Hovering either gizmo dims the other.

No gizmo axis depends on the camera. Orbit all the way around and the handles
mean exactly what they meant before — only their on-screen size changes.

Color legend: pale blue = selected; red = a subtracted shape's wireframe
(shown even unselected); surface = normal-colored debug (see Rendering below).

## Rendering

The viewport is a per-pixel sphere-traced render of the CSG scene
(`shaders/raymarch.metal` + `shaders/sdf_scene.h`), evaluated every frame —
edits and drags are visible live, with no reconstruction latency. Shading is
normal-colored debug (color = 0.5·(n+1), gradient normal — no lighting). The
wireframe draws for the selected object, plus any unselected Subtract node
(its carve is otherwise invisible) — unselected Add nodes draw none. The
depth buffer is real: the raymarch fragment shader writes true per-pixel
depth, so later passes composite correctly on top of it.

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
  controller, cursor-anchored navigation (`navigation.h`), SDF-traced picking,
  gizmo drag/scale math, DCSDD mesh reconstruction
  (dormant, background thread), and all Metal rendering (raymarch + wireframe
  + shaded-mesh pipelines, depth buffer, per-frame encode) via metal-cpp.
- **Swift** (`app/Sources`) owns the 2-mode state machine and the pointer
  gesture state machine, raw AppKit input
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
- A single rotation drag is capped at half a turn; past ±180° it reverses, and
  a further drag continues the spin. The tradeoff buys a gesture that is a pure
  function of cursor position, so it can never miscount a winding.
- Attachment is inert: a shape remembers the surface point it was spawned on,
  but moving its parent does not carry it, and dragging it onto a different
  surface does not re-attach it.
- No materials or lighting — shading is normal-colored debug only.
- No export/saving.
- Single window.
- Picking (and therefore the orbit pivot and the hover dot) sphere-traces each
  node's own SDF and ignores CSG, so a region carved away by a Subtract node
  still reports a hit. Hits land within the trace's epsilon of the surface
  rather than exactly on it, and a ray grazing a silhouette can exhaust its
  step budget and miss.
