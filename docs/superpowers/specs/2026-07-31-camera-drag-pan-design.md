# Camera-mode click-drag pan — design

Approved 2026-07-31. UI tweak: in camera mode, clicking and dragging in the
viewport pans the camera along a virtual plane orthogonal to the view
direction, as if dragging a point on a plane in front of the camera.

User rulings: grab-the-world semantics (the clicked point stays under the
cursor — drag right, scene follows right); drag-pan coexists with the existing
scroll-orbit / shift+scroll-pan / pinch-zoom gestures.

## 1. Reuse of existing core math (no core changes)

`CameraController::pan_view` (`core/src/camera_controller.cpp`) already
implements the requested behavior: it moves the orbit target along the
camera's right/up basis vectors — a plane orthogonal to the view — scaled by
`2·radius·tan(fov_y/2)/viewport_h_pts`, so content at target depth tracks the
cursor 1:1. Because the plane is orthogonal to the view direction, the
screen→plane mapping is linear; summing per-event mouse deltas times that
constant scale is mathematically identical to an explicit ray–plane
intersection drag. It is reached via the existing `Editor::cameraPan(dxPts,
dyPts)` interop call and is covered by `camera_controller_tests.cpp`.

Alternative rejected: new `beginCameraPan`/`updateCameraPan` core methods
doing explicit ray–plane intersection — identical result for a
view-orthogonal plane, more interop surface, duplicates tested math.

## 2. Swift wiring — `EditorViewModel` only

New private state: `var lastCameraDragPoint: CGPoint?` — the previous mouse
position of an in-progress camera drag, nil when no camera drag is active.

- `handleMouseDown`, `.camera` arm (currently a no-op): set
  `lastCameraDragPoint = p`.
- `handleMouseDragged`: when `mode == .camera` and `lastCameraDragPoint` is
  non-nil, compute `dx/dy = p − last`, call `editor.cameraPan(Float(dx),
  Float(dy))`, set `lastCameraDragPoint = p`, and call
  `refreshOverlayState()` (the radial-menu anchor mirrors camera moves, same
  as the scroll path).
- `handleMouseUp`: when `mode == .camera`, clear `lastCameraDragPoint`.

Sign convention: view coords are flipped (top-left origin), and `pan_view`
does `target += (−dx·right + dy·up)·scale`, so raw `p − last` deltas yield
grab-the-world tracking — no negation needed.

## 3. Edge cases

- **Mode switch mid-drag**: the optional anchor is the guard. Switching into
  `.camera` while the button is already down leaves `lastCameraDragPoint`
  nil, so drags are ignored until the next mouse-down (no huge first-delta
  jump). Switching out of `.camera` mid-drag needs no core cleanup —
  `cameraPan` is stateless per-event, unlike modify-mode's begin/end drag
  pair. `setMode` is untouched. A stale anchor left by an out-switch is
  overwritten by the next camera-mode mouse-down before any drag can read it.
- **Zero delta**: `pan_view` already returns false and skips the line-buffer
  invalidation; no Swift-side filtering needed.

## 4. Testing

No new core math, so no new C++ tests. The Swift app layer has no test
target; the wiring follows the existing handler patterns and is verified by
`xcodebuild` + running the app and exercising the gesture alongside
scroll-orbit and pinch-zoom.
